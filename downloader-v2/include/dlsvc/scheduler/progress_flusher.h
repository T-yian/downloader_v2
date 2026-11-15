#pragma once

// D2 进度节流落库：
//   下载回调里只改内存计数（atomic 累加，纳秒级）；
//   独立 flusher 线程周期性把 (downloaded_bytes + 分片账本) 批量写 MySQL。
//
// 为什么不能在回调里直接写库：256KB 一块、10MB/s 就是一秒 40 次写库，
// DB 会被打成热点——文件场景"每块刷盘"的习惯在 DB 场景必须换成节流窗口。
// 节流窗口 = 崩溃时最多丢多少秒进度，这是精度与 DB 压力的折衷。
//
// 失败语义：落库失败不推进 flushedBytes，下一轮自动补写——DB 断连期间
// 进度只"延迟持久化"，不"丢失"。

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "dlsvc/core/types.h"
#include "dlsvc/db/segment_dao.h"
#include "dlsvc/db/task_dao.h"

namespace dlsvc {

class ProgressFlusher {
public:
    using SnapshotFn = std::function<std::vector<TaskContextPtr>()>;

    // flushBytes = 0 表示只用时间窗口；intervalMs 为节流窗口
    ProgressFlusher(TaskDao& dao, SegmentDao& segDao, SnapshotFn fn, long long intervalMs,
                    long long flushBytes);
    ~ProgressFlusher();

    void start();
    void stop();

private:
    void loop();
    void flushOne(TaskContext& ctx, const std::vector<SegmentRow>& rows);

    TaskDao& dao_;
    SegmentDao& segDao_;
    SnapshotFn snapshot_;
    long long intervalMs_;
    long long flushBytes_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

} // namespace dlsvc
