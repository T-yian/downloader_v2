#pragma once

// TaskManager：调度层核心 = worker 线程池 + 阻塞队列 + 状态机 + 两级限流 + 对账
//
// 职责清单（对应设计决策编号）：
//   D1  全局并发上限 = worker 线程数；任务内部还有分片并发（内核的事）
//   D3  断点匹配键 url+save_path（resume_key 唯一索引）
//   D5  启动对账：downloading（worker 死了）→ pending 重新排队
//   D6  取消传播：改 DB 状态 + 置内存取消标志，内核块级协作退出
//   D7  全局限流=池大小；用户级=提交时刻按 (user_id,status) 索引统计

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dlsvc/blocking_queue.h"
#include "dlsvc/config.h"
#include "dlsvc/core/types.h"
#include "dlsvc/db/mysql_store.h"

namespace dlsvc {

class ProgressFlusher;

class TaskManager {
public:
    TaskManager(DbPool& pool, const ServiceConfig& cfg);
    ~TaskManager();

    // DB 冒烟 → 重启对账 → 起 flusher → 起 workers
    bool start(std::string* err = nullptr);
    // 关队列 → 置取消标志（让下载协程快速收尾）→ join workers → 停 flusher
    void stop();

    // ---- API 调用面（httpStatus 填 HTTP 状态码，err 填 JSON error 文案）----
    bool submit(const std::string& userId, const std::string& url, const std::string& savePathRaw,
                long long& outId, bool& resumed, int& httpStatus, std::string& err);
    bool cancel(long long id, int& httpStatus, std::string& err);
    bool getTask(long long id, TaskRow& out, std::vector<SegmentRow>& segs,
                 std::string* err = nullptr);
    bool listTasks(const std::string& status, const std::string& userId, int limit,
                   std::vector<TaskRow>& out, std::string* err = nullptr);

    void healthSnapshot(long long& activeTasks, size_t& queueSize, size_t& workerCount) const;
    std::vector<TaskContextPtr> activeSnapshot() const; // flusher 用

private:
    void workerLoop();
    void reconcile();
    TaskContextPtr findActive(long long id) const;
    void registerActive(const TaskContextPtr& ctx);
    void unregisterActive(long long id);

    DbPool& pool_;
    ServiceConfig cfg_;
    TaskDao dao_;
    SegmentDao segDao_;
    MysqlTaskStore store_;

    BlockingQueue<long long> queue_; // 队列只传 id；worker 弹出后以 DB 为准重读任务
    std::vector<std::thread> workers_;

    mutable std::mutex mu_;
    std::unordered_map<long long, TaskContextPtr> active_; // 进行中任务的内存态

    std::unique_ptr<ProgressFlusher> flusher_;
    std::atomic<bool> stopped_{false};
};

} // namespace dlsvc
