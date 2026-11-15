#include "dlsvc/scheduler/task_manager.h"

#include <algorithm>
#include <chrono>

#include "dlsvc/core/download_runner.h"
#include "dlsvc/log.h"
#include "dlsvc/scheduler/progress_flusher.h"
#include "dlsvc/util.h"

namespace dlsvc {

TaskManager::TaskManager(DbPool& pool, const ServiceConfig& cfg)
    : pool_(pool), cfg_(cfg), dao_(pool), segDao_(pool), store_(pool),
      queue_((size_t)std::max(1, cfg.queueCapacity)) {}

TaskManager::~TaskManager() { stop(); }

bool TaskManager::start(std::string* err) {
    // 先对 DB 做一次真实借还冒烟，配置错误在这里就暴露
    auto h = pool_.acquire(err, 5000);
    if (!h) {
        if (err && err->empty()) *err = "db unreachable";
        return false;
    }

    reconcile(); // D5：开机第一件事——把上一世的活接回来

    flusher_ = std::make_unique<ProgressFlusher>(dao_, segDao_,
                                                 [this] { return activeSnapshot(); },
                                                 cfg_.flushIntervalMs, cfg_.flushBytes);
    flusher_->start();

    for (int i = 0; i < cfg_.workerCount; ++i)
        workers_.emplace_back([this] { workerLoop(); });

    LOG_INFO("task manager started: workers=%d, queue_capacity=%d, max_active_per_user=%d",
             cfg_.workerCount, cfg_.queueCapacity, cfg_.maxActivePerUser);
    return true;
}

void TaskManager::stop() {
    if (stopped_.exchange(true)) return;
    queue_.close();
    {
        // 让所有在跑的下载协程尽快协作退出（D6），shutdown 不强杀线程
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : active_) kv.second->cancelRequested.store(true);
    }
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    if (flusher_) flusher_->stop();
    LOG_INFO("task manager stopped");
}

// ---------------- D5 重启对账 ----------------

void TaskManager::reconcile() {
    std::vector<TaskRow> rows;
    if (!dao_.listForReconcile(rows)) {
        LOG_ERROR("reconcile: query failed, skip");
        return;
    }
    long long reset = 0, queued = 0;
    for (auto& r : rows) {
        if (r.status == TaskStatus::Downloading) {
            // worker 已经不在了，downloading 是残留态 → 重置 pending
            // （文件与分片账本都在，重新排队后从断点继续）
            long long affected = 0;
            dao_.updateStatusFrom(r.id, TaskStatus::Pending, "reconciled on restart", -1, -1,
                                  {TaskStatus::Downloading}, affected);
            ++reset;
        }
        // pending 也要重新入队（提交后、worker 处理前崩掉的任务，队列在内存里已丢）
        if (queue_.tryPush(r.id, std::chrono::milliseconds(100))) {
            ++queued;
        } else {
            LOG_ERROR("reconcile: queue full, task %lld left pending (won't run until resubmit)",
                      r.id);
        }
    }
    LOG_INFO("reconcile: %zu rows, %lld reset downloading->pending, %lld queued", rows.size(),
             reset, queued);
}

// ---------------- D7 两级限流 + 提交 ----------------

bool TaskManager::submit(const std::string& userId, const std::string& url,
                         const std::string& savePathRaw, long long& outId, bool& resumed,
                         int& httpStatus, std::string& err) {
    resumed = false;

    // 1) 业务校验（接入层已做一遍格式检查，这里做语义校验 + 路径收口）
    if (!util::isHttpUrl(url)) {
        httpStatus = 400;
        err = "url must be http(s)";
        return false;
    }
    std::string savePath;
    if (!util::normalizeSavePath(cfg_.storageRoot, savePathRaw, cfg_.restrictToRoot, savePath, err)) {
        httpStatus = 400;
        return false;
    }

    // 2) D3 断点匹配：url+save_path 已有记录 → 视状态复活或拒绝
    const std::string key = util::makeResumeKey(url, savePath);
    TaskRow existing;
    bool found = false;
    if (!dao_.getByResumeKey(key, existing, found)) {
        httpStatus = 500;
        err = "db error";
        return false;
    }
    if (found) {
        switch (existing.status) {
            case TaskStatus::Success:
                httpStatus = 409;
                err = "same url+save_path task already succeeded, id=" + std::to_string(existing.id);
                return false;
            case TaskStatus::Downloading:
                httpStatus = 409;
                err = "task already downloading, id=" + std::to_string(existing.id);
                return false;
            case TaskStatus::Pending:
                outId = existing.id;
                resumed = true;
                httpStatus = 200;
                return true; // 已在队列里，不重复入队
            case TaskStatus::Failed:
            case TaskStatus::Canceled: {
                // 复活：回 pending，账本/文件都保留 → 断点续传（D6 可续传语义）
                long long affected = 0;
                if (!dao_.updateStatusFrom(existing.id, TaskStatus::Pending, "", -1, -1,
                                           {TaskStatus::Failed, TaskStatus::Canceled}, affected) ||
                    affected == 0) {
                    httpStatus = 409;
                    err = "task state changed, retry later";
                    return false;
                }
                if (!queue_.push(existing.id)) {
                    httpStatus = 503;
                    err = "queue closed";
                    return false;
                }
                outId = existing.id;
                resumed = true;
                httpStatus = 200;
                LOG_INFO("task %lld resumed from %s (downloaded=%s)", existing.id,
                         toString(existing.status),
                         util::humanBytes(existing.downloadedBytes).c_str());
                return true;
            }
        }
    }

    // 3) D7 用户级限流：提交时刻检查（内存计数 vs 查 DB 选了查 DB——天然精确，
    //    且命中 (user_id,status) 索引，代价是一次索引扫描）
    long long active = 0;
    if (!dao_.countActiveByUser(userId, active)) {
        httpStatus = 500;
        err = "db error";
        return false;
    }
    if (active >= cfg_.maxActivePerUser) {
        httpStatus = 429;
        err = "user active task limit reached (" + std::to_string(cfg_.maxActivePerUser) + ")";
        return false;
    }

    // 4) 入库 pending → 入队；队列满 → 回滚记录 → 503（全局容量保护）
    TaskRow row;
    row.userId = userId;
    row.url = url;
    row.savePath = savePath;
    row.resumeKey = key;
    if (!dao_.create(row)) {
        httpStatus = 500;
        err = "db error";
        return false;
    }
    if (!queue_.tryPush(row.id, std::chrono::milliseconds(200))) {
        dao_.deletePending(row.id);
        httpStatus = 503;
        err = "queue full";
        return false;
    }
    outId = row.id;
    httpStatus = 200;
    LOG_INFO("task %lld submitted: user=%s url=%s save=%s", row.id, userId.c_str(), url.c_str(),
             savePath.c_str());
    return true;
}

// ---------------- D6 取消传播 ----------------

bool TaskManager::cancel(long long id, int& httpStatus, std::string& err) {
    TaskRow row;
    if (!dao_.getById(id, row) || row.id == 0) {
        httpStatus = 404;
        err = "task not found";
        return false;
    }
    if (isTerminal(row.status)) {
        httpStatus = 409;
        err = "task already terminal: " + std::string(toString(row.status));
        return false;
    }

    if (row.status == TaskStatus::Downloading) {
        auto ctx = findActive(id);
        if (ctx) ctx->cancelRequested.store(true); // 内核在下一个写盘块退出
    }
    // 条件更新：只有还处于非终态才写 canceled —— 与 worker 的终态写入互斥，
    // 谁先迁移成功谁生效，不会出现 success 被 canceled 覆盖的回退
    long long affected = 0;
    if (dao_.updateStatusFrom(id, TaskStatus::Canceled, "canceled by user", -1, -1,
                              {TaskStatus::Pending, TaskStatus::Downloading}, affected) &&
        affected > 0) {
        httpStatus = 200;
        return true;
    }
    httpStatus = 409;
    err = "task state changed, retry later";
    return false;
}

// ---------------- 查询 ----------------

bool TaskManager::getTask(long long id, TaskRow& out, std::vector<SegmentRow>& segs,
                          std::string* err) {
    if (!dao_.getById(id, out, err) || out.id == 0) return false;

    auto ctx = findActive(id);
    if (ctx) {
        // 进行中：内存态比 DB 新（D2 节流期间 DB 落后于真实进度）
        out.status = ctx->status.load();
        out.totalSize = ctx->totalSize.load();
        out.downloadedBytes = ctx->downloadedBytes.load();
        out.retryCount = ctx->retryCount.load();
        std::lock_guard<std::mutex> lk(ctx->mu_);
        out.failReason = ctx->failReason;
        for (const auto& s : ctx->segments)
            segs.push_back({s->segIndex, s->segStart, s->segEnd, s->downloaded.load(),
                            s->finished.load()});
        return true;
    }
    // 不在跑：分片账本从 DB 读（failed/canceled 的可续传状态也能看到断点）
    segDao_.loadByTask(id, segs);
    return true;
}

bool TaskManager::listTasks(const std::string& status, const std::string& userId, int limit,
                            std::vector<TaskRow>& out, std::string* err) {
    return dao_.listTasks(status, userId, limit, out);
}

void TaskManager::healthSnapshot(long long& activeTasks, size_t& queueSize,
                                 size_t& workerCount) const {
    std::lock_guard<std::mutex> lk(mu_);
    activeTasks = (long long)active_.size();
    queueSize = queue_.size();
    workerCount = workers_.size();
}

std::vector<TaskContextPtr> TaskManager::activeSnapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TaskContextPtr> out;
    out.reserve(active_.size());
    for (auto& kv : active_) out.push_back(kv.second);
    return out;
}

TaskContextPtr TaskManager::findActive(long long id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = active_.find(id);
    return it == active_.end() ? nullptr : it->second;
}

void TaskManager::registerActive(const TaskContextPtr& ctx) {
    std::lock_guard<std::mutex> lk(mu_);
    active_[ctx->id] = ctx;
}

void TaskManager::unregisterActive(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    active_.erase(id);
}

// ---------------- worker 主循环 ----------------

void TaskManager::workerLoop() {
    long long id = 0;
    // pop 阻塞等待；close 后存量取完返回 false → 线程池干净退出
    while (!stopped_.load() && queue_.pop(id)) {
        TaskRow row;
        if (!dao_.getById(id, row) || row.id == 0) {
            // DB 暂时不可用 ≠ 任务消失：退避后重新入队，等 DB 恢复再拾起。
            // （重启对账 D5 管的是"进程崩溃"，这里管的是"运行期 DB 抖动"——
            //   不重入队的话，DB 断连窗口里排队的任务会被 worker 静默跳过）
            if (!stopped_.load()) {
                LOG_WARN("worker: task %lld not readable (db down?), requeue after backoff", id);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                queue_.tryPush(id, std::chrono::milliseconds(100));
            }
            continue;
        }
        // 取消竞态兜底：排队中取消只改了 DB，任务弹出时在这里按终态跳过
        if (isTerminal(row.status)) continue;

        auto ctx = std::make_shared<TaskContext>();
        ctx->id = row.id;
        ctx->userId = row.userId;
        ctx->url = row.url;
        ctx->savePath = row.savePath;
        ctx->totalSize.store(row.totalSize);
        ctx->retryCount.store(row.retryCount);
        registerActive(ctx);

        // pending -> downloading 也是条件迁移：cancel 若抢先把 pending 改成
        // canceled，这里 affected=0，直接跳过
        long long affected = 0;
        bool ok = dao_.updateStatusFrom(id, TaskStatus::Downloading, "", -1, -1,
                                        {TaskStatus::Pending}, affected) &&
                  affected > 0;
        if (!ok) {
            unregisterActive(id);
            continue;
        }
        ctx->status.store(TaskStatus::Downloading);
        store_.setTaskRunning(id);

        // 下载内核执行（含探测/分片/重试/落账/取消）
        DownloadRunner runner(cfg_.download, store_);
        std::string reason;
        auto outcome = runner.run(ctx, reason);

        // 终态写入带条件（防与 cancel 竞态回退）
        TaskStatus finalSt = outcome == DownloadOutcome::Success  ? TaskStatus::Success
                             : outcome == DownloadOutcome::Canceled ? TaskStatus::Canceled
                                                                    : TaskStatus::Failed;
        // reason 同步进内存态：API 在 setTaskTerminal 与 unregister 之间的窗口
        // 读的是内存 ctx，漏了这一步会出现 status=failed 但 fail_reason 为空
        if (!reason.empty()) {
            std::lock_guard<std::mutex> lk(ctx->mu_);
            ctx->failReason = reason;
        }
        store_.setTaskTerminal(id, finalSt, reason, ctx->downloadedBytes.load(),
                               ctx->retryCount.load(), {TaskStatus::Downloading});
        unregisterActive(id);
        LOG_INFO("task %lld -> %s (%s)", id, toString(finalSt), reason.c_str());
    }
}

} // namespace dlsvc
