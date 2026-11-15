#pragma once

// 进程内 TaskStore：dlcli 与单测用，零依赖。

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "dlsvc/core/task_store.h"

namespace dlsvc {

class MemoryTaskStore : public TaskStore {
public:
    bool loadSegments(long long taskId, std::vector<SegmentRow>& out) override;
    bool replaceSegments(long long taskId, const std::vector<SegmentRow>& plan) override;
    bool saveSegments(long long taskId, const std::vector<SegmentRow>& snapshot) override;
    bool removeSegments(long long taskId) override;
    bool setTaskRunning(long long taskId) override;
    bool setTaskTotalSize(long long taskId, long long totalSize) override;
    bool setTaskTerminal(long long taskId, TaskStatus st, const std::string& failReason,
                         long long downloadedBytes, int retryCount,
                         const std::vector<TaskStatus>& from) override;

private:
    std::mutex mu_;
    std::map<long long, std::vector<SegmentRow>> segs_;
};

} // namespace dlsvc
