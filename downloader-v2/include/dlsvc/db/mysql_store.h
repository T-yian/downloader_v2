#pragma once

// TaskStore 的 MySQL 实现：把内核面向的持久化接口落到双表账本上。
// 终态写入沿用 TaskDao 的条件更新语义（防 cancel 竞态）；Success 时顺带清分片账本。

#include "dlsvc/core/task_store.h"
#include "dlsvc/db/segment_dao.h"
#include "dlsvc/db/task_dao.h"

namespace dlsvc {

class MysqlTaskStore : public TaskStore {
public:
    explicit MysqlTaskStore(DbPool& pool) : dao_(pool), segDao_(pool) {}

    bool loadSegments(long long taskId, std::vector<SegmentRow>& out) override {
        return segDao_.loadByTask(taskId, out);
    }
    bool replaceSegments(long long taskId, const std::vector<SegmentRow>& plan) override {
        return segDao_.replaceAll(taskId, plan);
    }
    bool saveSegments(long long taskId, const std::vector<SegmentRow>& snapshot) override {
        return segDao_.upsertProgress(taskId, snapshot);
    }
    bool removeSegments(long long taskId) override { return segDao_.removeByTask(taskId); }

    bool setTaskRunning(long long taskId) override {
        long long affected = 0;
        return dao_.updateStatusFrom(taskId, TaskStatus::Downloading, "", -1, -1,
                                     {TaskStatus::Pending}, affected) &&
               affected > 0;
    }
    bool setTaskTotalSize(long long taskId, long long totalSize) override {
        return dao_.updateProgress(taskId, -1, totalSize);
    }
    bool setTaskTerminal(long long taskId, TaskStatus st, const std::string& failReason,
                         long long downloadedBytes, int retryCount,
                         const std::vector<TaskStatus>& from) override {
        long long affected = 0;
        if (!dao_.updateStatusFrom(taskId, st, failReason, downloadedBytes, retryCount, from,
                                   affected))
            return false;
        if (st == TaskStatus::Success && affected > 0) return segDao_.removeByTask(taskId);
        return true;
    }

private:
    TaskDao dao_;
    SegmentDao segDao_;
};

} // namespace dlsvc
