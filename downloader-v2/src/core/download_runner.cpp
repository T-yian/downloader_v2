#include "dlsvc/core/download_runner.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace dlsvc {

namespace {

// 内存分片快照 → 账本行（downloaded/finished 是 atomic，这里读一致性快照）
void snapshotSegments(TaskContext& ctx, std::vector<SegmentRow>& out) {
    std::lock_guard<std::mutex> lk(ctx.mu_);
    for (const auto& s : ctx.segments)
        out.push_back({s->segIndex, s->segStart, s->segEnd, s->downloaded.load(),
                       s->finished.load()});
}

} // namespace

DownloadRunner::DownloadRunner(DownloadConfig cfg, TaskStore& store)
    : cfg_(std::move(cfg)), store_(store) {}

// 写回调：libcurl 每收到一块数据进来一次。
// D6 协作式取消的检查点就在这里——每块写盘前先看取消标志，命中立刻返回 0
//（libcurl 视为写失败终止传输），线程随后退出，文件与账本停在断点处。
size_t DownloadRunner::writeCb(char* data, size_t size, size_t n, void* ud) {
    auto* job = static_cast<SegJob*>(ud);
    size_t bytes = size * n;
    if (bytes == 0) return 0;
    if (job->ctx->cancelRequested.load(std::memory_order_relaxed)) return 0;

    // pwrite + 绝对偏移：同一预分配文件、各分片各写各的区间，天然无锁
    long long offset = job->seg->segStart + job->seg->downloaded.load(std::memory_order_relaxed);
    const char* p = data;
    size_t left = bytes;
    while (left > 0) {
        ssize_t w = ::pwrite(job->fd, p, left, (off_t)offset);
        if (w < 0) {
            if (errno == EINTR) continue;
            job->error = util::stringPrintf("seg%d pwrite@%lld: %s", job->seg->segIndex, offset,
                                            strerror(errno));
            return 0;
        }
        offset += w;
        p += w;
        left -= (size_t)w;
    }
    job->seg->downloaded.fetch_add((long long)bytes, std::memory_order_relaxed);
    job->ctx->downloadedBytes.fetch_add((long long)bytes, std::memory_order_relaxed);
    return bytes;
}

bool DownloadRunner::backoffSleep(TaskContext& ctx, long long ms) {
    long long slept = 0;
    while (slept < ms) {
        if (ctx.cancelRequested.load(std::memory_order_relaxed)) return false;
        long long step = std::min<long long>(100, ms - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        slept += step;
    }
    return !ctx.cancelRequested.load(std::memory_order_relaxed);
}

// 决定分片计划并准备 .dltmp：
//   分片模式：ftruncate 预分配 + 复用/重建 DB 分片账本
//   降级模式：单分片，从已有 .dltmp 文件大小续传（服务器不给 Range，只能追加语义）
bool DownloadRunner::planAndPrepare(TaskContext& ctx, const ProbeResult& pr, bool degraded,
                                    int& outFd, std::string& err) {
    outFd = -1;
    const std::string tmpPath = ctx.savePath + ".dltmp";

    if (degraded) {
        long long fsize = 0;
        util::fileSize(tmpPath, fsize);
        int fd = ::open(tmpPath.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            err = util::stringPrintf("open tmp failed: %s (%s)", tmpPath.c_str(), strerror(errno));
            return false;
        }
        auto seg = std::make_shared<SegmentProgress>();
        seg->segIndex = 0;
        seg->segStart = 0;
        seg->segEnd = pr.contentLength > 0 ? pr.contentLength - 1 : -1;
        seg->downloaded.store(fsize);
        {
            std::lock_guard<std::mutex> lk(ctx.mu_);
            ctx.segments = {seg};
        }
        ctx.downloadedBytes.store(fsize);
        std::vector<SegmentRow> rows;
        snapshotSegments(ctx, rows);
        store_.replaceSegments(ctx.id, rows);
        outFd = fd;
        return true;
    }

    const long long total = pr.contentLength;
    auto plan = util::splitSegments(total, cfg_.maxSegmentsPerTask, cfg_.minSegmentSize);

    std::vector<SegmentRow> persisted;
    store_.loadSegments(ctx.id, persisted);
    long long fsize = -1;
    util::fileSize(tmpPath, fsize);

    // 复用旧账本的前提：计划形状一致 + 临时文件尺寸对得上 + 每行进度合法
    bool reuse = persisted.size() == plan.size() && fsize == total;
    if (reuse) {
        for (size_t i = 0; i < plan.size(); ++i) {
            const SegmentRow& r = persisted[i];
            long long len = plan[i].second - plan[i].first + 1;
            if (r.segIndex != (int)i || r.segStart != plan[i].first || r.segEnd != plan[i].second ||
                r.downloaded < 0 || r.downloaded > len || (r.finished && r.downloaded != len)) {
                reuse = false;
                break;
            }
        }
    }

    std::vector<SegmentRow> init;
    int fd;
    if (reuse) {
        fd = ::open(tmpPath.c_str(), O_RDWR);
        if (fd < 0) reuse = false;
        else init = persisted;
    }
    if (!reuse) {
        // 新计划：O_TRUNC 清掉旧数据再 ftruncate 预分配到目标大小（稀疏文件）
        fd = ::open(tmpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            err = util::stringPrintf("open/truncate tmp failed: %s (%s)", tmpPath.c_str(),
                                     strerror(errno));
            return false;
        }
        if (::ftruncate(fd, total) != 0) {
            err = util::stringPrintf("ftruncate failed: %s", strerror(errno));
            ::close(fd);
            return false;
        }
        init.clear();
        for (size_t i = 0; i < plan.size(); ++i)
            init.push_back({(int)i, plan[i].first, plan[i].second, 0, false});
        store_.replaceSegments(ctx.id, init); // 覆盖旧账本（url 相同但服务器文件已变的情形）
    }

    long long sum = 0;
    std::vector<std::shared_ptr<SegmentProgress>> segs;
    for (const auto& r : init) {
        auto s = std::make_shared<SegmentProgress>();
        s->segIndex = r.segIndex;
        s->segStart = r.segStart;
        s->segEnd = r.segEnd;
        s->downloaded.store(r.downloaded);
        s->finished.store(r.finished);
        sum += r.downloaded;
        segs.push_back(std::move(s));
    }
    {
        std::lock_guard<std::mutex> lk(ctx.mu_);
        ctx.segments = std::move(segs);
    }
    ctx.downloadedBytes.store(sum);
    outFd = fd;
    return true;
}

// 下载一个分片（本分片一次传输尝试）。返回空串=成功；否则返回错误描述。
std::string DownloadRunner::downloadOne(TaskContext& ctx, int fd, SegmentProgress& seg,
                                        bool sendRange, bool& permanent) {
    permanent = false;
    const long long len = seg.length();
    const long long prev = seg.downloaded.load(std::memory_order_relaxed);
    if (seg.segEnd >= 0 && prev >= len) { // 上轮写满但没来得及标 finished（崩溃窗口）→ 直接补标
        seg.finished.store(true);
        return "";
    }
    const long long from = seg.segStart + prev;
    std::string range;
    if (sendRange && seg.segEnd >= 0) range = util::stringPrintf("%lld-%lld", from, seg.segEnd);

    CURL* e = curl_easy_init();
    if (!e) return "curl_easy_init failed";
    SegJob job{fd, &ctx, &seg, ""};
    curlSetCommon(e, ctx.url, cfg_.connectTimeoutS, cfg_.userAgent, cfg_.stallSpeedBps,
                  cfg_.stallTimeS);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &DownloadRunner::writeCb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &job);
    if (!range.empty()) curl_easy_setopt(e, CURLOPT_RANGE, range.c_str());

    CURLcode rc = curl_easy_perform(e);
    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(e);

    if (ctx.cancelRequested.load(std::memory_order_relaxed)) return ""; // 上层按取消收尾
    if (rc != CURLE_OK) {
        if (rc == CURLE_WRITE_ERROR && !job.error.empty()) {
            permanent = true; // 磁盘写失败重试也没用
            return job.error;
        }
        if (rc == CURLE_WRITE_ERROR) return "write aborted";
        return util::stringPrintf("seg%d curl(%d): %s @%lld", seg.segIndex, (int)rc,
                                  curl_easy_strerror(rc), prev);
    }

    const long long expect = len - prev; // 本轮应接收的字节数
    const long long got = seg.downloaded.load(std::memory_order_relaxed) - prev;

    if (range.empty()) {
        // 未带 Range 的整流下载（降级模式）：2xx 即可，连接干净结束=传输完成
        if (code < 200 || code >= 300) {
            if (code >= 400 && code < 500 && code != 408 && code != 429) permanent = true;
            return util::stringPrintf("seg%d http_%ld", seg.segIndex, code);
        }
        if (seg.segEnd < 0) { // 总长未知：EOF 即完成
            seg.finished.store(true);
            return "";
        }
        if (got != expect)
            return util::stringPrintf("seg%d short read: got=%lld expect=%lld", seg.segIndex, got,
                                      expect);
        seg.finished.store(true);
        return "";
    }

    // 带 Range 的分片请求：必须 206；服务器谎报支持 Range（对分片回 200）会写坏文件，
    // 所以只有"单分片且起点为 0"这一种无害情形容忍 200。
    bool ok206 = code == 206;
    bool lenient200 = code == 200 && seg.segStart == 0 && ctx.segments.size() == 1;
    if (!ok206 && !lenient200) {
        if (code == 200) {
            permanent = true;
            return "server ignored Range (200 for ranged request)";
        }
        if (code >= 400 && code < 500 && code != 408 && code != 429) permanent = true;
        return util::stringPrintf("seg%d http_%ld (expect 206)", seg.segIndex, code);
    }
    if (got != expect)
        return util::stringPrintf("seg%d short read: got=%lld expect=%lld", seg.segIndex, got,
                                  expect);
    seg.finished.store(true);
    return "";
}

// 一轮并发：所有未完成分片各起一个线程（连接数 = 未完成分片数）。
bool DownloadRunner::runSegmentPass(TaskContext& ctx, int fd, bool degraded, std::string& err,
                                    bool& permanentFail) {
    permanentFail = false;
    std::vector<SegmentProgress*> todo;
    {
        std::lock_guard<std::mutex> lk(ctx.mu_);
        for (const auto& s : ctx.segments)
            if (!s->finished.load()) todo.push_back(s.get());
    }
    if (todo.empty()) return true;

    struct SegResult {
        std::string error;
        bool permanent = false;
    };
    std::vector<SegResult> results(todo.size());
    std::vector<std::thread> threads;
    threads.reserve(todo.size());
    for (size_t i = 0; i < todo.size(); ++i) {
        SegmentProgress* seg = todo[i];
        threads.emplace_back([this, &ctx, fd, seg, degraded, &results, i] {
            results[i].error = downloadOne(ctx, fd, *seg, !degraded, results[i].permanent);
        });
    }
    for (auto& t : threads) t.join();

    if (ctx.cancelRequested.load(std::memory_order_relaxed)) {
        err = "canceled";
        return false;
    }

    // 每轮结束落一次账（另有 flusher 周期落账，这里兜底把"轮"粒度的进度固化）
    std::vector<SegmentRow> snap;
    snapshotSegments(ctx, snap);
    store_.saveSegments(ctx.id, snap);

    for (const auto& r : results) {
        if (r.error.empty()) continue;
        if (!err.empty()) err += " | ";
        err += r.error;
        if (r.permanent) permanentFail = true;
    }
    if (err.size() > 500) err = err.substr(0, 500);
    if (!err.empty()) return false;

    // 校验：全部分片必须 finished 且字节数精确匹配
    {
        std::lock_guard<std::mutex> lk(ctx.mu_);
        for (const auto& s : ctx.segments) {
            if (!s->finished.load()) {
                err = util::stringPrintf("seg%d incomplete after pass", s->segIndex);
                return false;
            }
        }
    }
    return true;
}

// 重试外环：瞬时失败自动重试（退避递增），永久失败（4xx/Range 被无视）直接判死
bool DownloadRunner::runLoop(TaskContext& ctx, int fd, bool degraded, std::string& err) {
    std::string lastErr;
    for (long long attempt = 0; attempt <= cfg_.maxRetries; ++attempt) {
        if (ctx.cancelRequested.load(std::memory_order_relaxed)) {
            err = "canceled";
            return false;
        }
        if (attempt > 0) {
            ctx.retryCount.store((int)attempt);
            LOG_WARN("task %lld: retry %lld/%lld after: %s", ctx.id, attempt, cfg_.maxRetries,
                     lastErr.c_str());
            if (!backoffSleep(ctx, cfg_.retryBackoffMs * attempt)) {
                err = "canceled";
                return false;
            }
        }
        bool permanent = false;
        if (runSegmentPass(ctx, fd, degraded, lastErr, permanent)) return true;
        if (ctx.cancelRequested.load(std::memory_order_relaxed)) {
            err = "canceled";
            return false;
        }
        if (permanent) break;
    }
    err = lastErr.empty() ? "unknown download failure" : lastErr;
    return false;
}

// 收尾：fsync → rename 原子发布 → 目录 fsync（掉电级别可靠性）
bool DownloadRunner::finalize(TaskContext& ctx, int fd, std::string& err) {
    if (::fsync(fd) != 0) {
        err = util::stringPrintf("fsync: %s", strerror(errno));
        return false;
    }
    const std::string tmpPath = ctx.savePath + ".dltmp";
    if (::rename(tmpPath.c_str(), ctx.savePath.c_str()) != 0) {
        err = util::stringPrintf("rename %s -> %s: %s", tmpPath.c_str(), ctx.savePath.c_str(),
                                 strerror(errno));
        return false;
    }
    int dfd = ::open(util::dirName(ctx.savePath).c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    store_.removeSegments(ctx.id); // 成功终态：分片账本使命完成
    return true;
}

DownloadOutcome DownloadRunner::run(TaskContextPtr ctxPtr, std::string& failReason) {
    TaskContext& ctx = *ctxPtr;
    curlGlobalInitOnce();
    failReason.clear();

    if (ctx.cancelRequested.load(std::memory_order_relaxed)) {
        ctx.status.store(TaskStatus::Canceled);
        return DownloadOutcome::Canceled;
    }
    if (!util::mkdirs(util::dirName(ctx.savePath))) {
        failReason = "mkdir failed: " + util::dirName(ctx.savePath);
        ctx.status.store(TaskStatus::Failed);
        return DownloadOutcome::Failed;
    }

    HttpClient hc(cfg_.connectTimeoutS);
    ProbeResult pr = hc.probe(ctx.url);
    if (!pr.ok) {
        failReason = "probe failed: " +
                     (pr.error.empty() ? util::stringPrintf("http_%ld", pr.httpCode) : pr.error);
        ctx.status.store(TaskStatus::Failed);
        return DownloadOutcome::Failed;
    }
    LOG_INFO("task %lld: probe ok, total=%s, ranges=%d", ctx.id,
             util::humanBytes(pr.contentLength).c_str(), (int)pr.acceptRanges);
    if (ctx.cancelRequested.load(std::memory_order_relaxed)) {
        ctx.status.store(TaskStatus::Canceled);
        return DownloadOutcome::Canceled;
    }

    if (pr.contentLength >= 0 && ctx.totalSize.load() != pr.contentLength) {
        ctx.totalSize.store(pr.contentLength);
        store_.setTaskTotalSize(ctx.id, pr.contentLength);
    }

    // 降级条件：不支持 Range，或总长未知（无法切分片）
    const bool degraded = !pr.acceptRanges || pr.contentLength < 0;
    if (degraded)
        LOG_INFO("task %lld: degrade to single stream (ranges=%d total=%lld)", ctx.id,
                 (int)pr.acceptRanges, pr.contentLength);

    int fd = -1;
    std::string err;
    if (!planAndPrepare(ctx, pr, degraded, fd, err)) {
        failReason = err;
        ctx.status.store(TaskStatus::Failed);
        return DownloadOutcome::Failed;
    }

    DownloadOutcome outcome;
    if (runLoop(ctx, fd, degraded, err)) {
        outcome = DownloadOutcome::Success;
    } else {
        outcome = ctx.cancelRequested.load() ? DownloadOutcome::Canceled : DownloadOutcome::Failed;
    }

    if (outcome == DownloadOutcome::Canceled) {
        // 可续传语义：.dltmp 与分片账本都保留，同 url+save_path 再提交即从断点接着下
        ::close(fd);
        ctx.status.store(TaskStatus::Canceled);
        std::vector<SegmentRow> snap;
        snapshotSegments(ctx, snap);
        store_.saveSegments(ctx.id, snap);
        return DownloadOutcome::Canceled;
    }
    if (outcome == DownloadOutcome::Failed) {
        ::close(fd);
        failReason = err;
        ctx.status.store(TaskStatus::Failed);
        return DownloadOutcome::Failed;
    }
    // 成功收尾：fsync(fd) → rename 原子发布（fd 在 finalize 之后才关）
    if (!finalize(ctx, fd, err)) {
        ::close(fd);
        failReason = err;
        ctx.status.store(TaskStatus::Failed);
        return DownloadOutcome::Failed;
    }
    ::close(fd);
    ctx.status.store(TaskStatus::Success);
    return DownloadOutcome::Success;
}

} // namespace dlsvc
