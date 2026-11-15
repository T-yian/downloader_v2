#pragma once

// 任务持久化接口：DownloadRunner 只认它，不直接碰 MySQL。
//   服务端 = MysqlTaskStore（task + segment_progress 双表账本）
//   CLI/单测 = MemoryTaskStore（同构语义的进程内账本，无 DB 依赖）
//
// 抽象的价值：内核的"断点续传逻辑"与"账本介质"解耦——换账本（DB/文件/内存）
// 不需要动内核一行代码，也让内核可以被单测覆盖。

#include <string>
#include <vector>

#include "dlsvc/core/types.h"

namespace dlsvc {

class TaskStore {
public:
    virtual ~TaskStore() = default;

    // 读分片账本（无记录返回空且 ok）
    virtual bool loadSegments(long long taskId, std::vector<SegmentRow>& out) = 0;
    // 计划变更时整体替换（先删后写）
    virtual bool replaceSegments(long long taskId, const std::vector<SegmentRow>& plan) = 0;
    // 增量落账（upsert：进度 + finished）
    virtual bool saveSegments(long long taskId, const std::vector<SegmentRow>& snapshot) = 0;
    // 清账（成功终态时调用）
    virtual bool removeSegments(long long taskId) = 0;

    virtual bool setTaskRunning(long long taskId) = 0;
    virtual bool setTaskTotalSize(long long taskId, long long totalSize) = 0;
    // 终态写入带状态前置条件（from）：只有当前状态仍在 from 集合内才生效。
    // 这是防"cancel 与 worker 收尾竞态"的关键：两边都只从非终态迁出，谁先谁生效。
    virtual bool setTaskTerminal(long long taskId, TaskStatus st, const std::string& failReason,
                                 long long downloadedBytes, int retryCount,
                                 const std::vector<TaskStatus>& from) = 0;
};

} // namespace dlsvc
