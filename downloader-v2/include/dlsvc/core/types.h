#pragma once

// 领域类型：任务状态机 / 任务行 / 分片账本行 / 任务内存上下文

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace dlsvc {

// 状态机: pending -> downloading -> success / failed / canceled
// 取消只发生在 pending / downloading；终态不可再迁移（写入都带条件更新，见 TaskDao）
enum class TaskStatus { Pending, Downloading, Success, Failed, Canceled };

inline const char* toString(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending: return "pending";
        case TaskStatus::Downloading: return "downloading";
        case TaskStatus::Success: return "success";
        case TaskStatus::Failed: return "failed";
        case TaskStatus::Canceled: return "canceled";
    }
    return "unknown";
}

inline bool parseStatus(const std::string& s, TaskStatus& out) {
    if (s == "pending") { out = TaskStatus::Pending; return true; }
    if (s == "downloading") { out = TaskStatus::Downloading; return true; }
    if (s == "success") { out = TaskStatus::Success; return true; }
    if (s == "failed") { out = TaskStatus::Failed; return true; }
    if (s == "canceled") { out = TaskStatus::Canceled; return true; }
    return false;
}

inline bool isTerminal(TaskStatus s) {
    return s == TaskStatus::Success || s == TaskStatus::Failed || s == TaskStatus::Canceled;
}

// task 表的一行（DB 是任务事实的单一来源；内存态只是缓存加速）
struct TaskRow {
    long long id = 0;
    std::string userId;
    std::string url;
    std::string savePath;
    std::string resumeKey;
    long long totalSize = -1;
    long long downloadedBytes = 0;
    TaskStatus status = TaskStatus::Pending;
    std::string failReason;
    int retryCount = 0;
    std::string createdAt;
    std::string updatedAt;
};

// segment_progress 表的一行；区间为闭区间 [segStart, segEnd]
struct SegmentRow {
    int segIndex = 0;
    long long segStart = 0;
    long long segEnd = 0;      // 降级单流且总长未知时为 -1
    long long downloaded = 0;  // 相对 segStart 的已下载偏移
    bool finished = false;
};

// 内存态分片进度：worker 多线程并发累加、flusher/API 只读快照 -> atomic
struct SegmentProgress {
    int segIndex = 0;
    long long segStart = 0;
    long long segEnd = 0;
    std::atomic<long long> downloaded{0};
    std::atomic<bool> finished{false};

    long long length() const { return segEnd - segStart + 1; }
};

// 任务内存上下文：worker / runner / flusher / API 共享的"活状态"
struct TaskContext {
    long long id = 0;
    std::string userId;
    std::string url;
    std::string savePath;

    std::atomic<TaskStatus> status{TaskStatus::Pending};
    std::atomic<long long> totalSize{-1};
    std::atomic<long long> downloadedBytes{0};
    std::atomic<int> retryCount{0};
    std::atomic<bool> cancelRequested{false}; // D6: 协作式取消标志

    std::string failReason;                   // mu_ 保护
    std::vector<std::shared_ptr<SegmentProgress>> segments; // mu_ 保护（计划确定后只读）
    mutable std::mutex mu_;

    // 节流记账（仅 flusher 线程读写）
    long long flushedBytes = 0;
    std::chrono::steady_clock::time_point lastFlushTime{};
};

using TaskContextPtr = std::shared_ptr<TaskContext>;

} // namespace dlsvc
