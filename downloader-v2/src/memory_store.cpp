#include "dlsvc/core/memory_store.h"

namespace dlsvc {

bool MemoryTaskStore::loadSegments(long long taskId, std::vector<SegmentRow>& out) {
    out.clear(); // 契约：无记录返回空 —— 不能把调用方传入的旧数据留着
    std::lock_guard<std::mutex> lk(mu_);
    auto it = segs_.find(taskId);
    if (it != segs_.end()) out = it->second;
    return true;
}

bool MemoryTaskStore::replaceSegments(long long taskId, const std::vector<SegmentRow>& plan) {
    std::lock_guard<std::mutex> lk(mu_);
    segs_[taskId] = plan;
    return true;
}

bool MemoryTaskStore::saveSegments(long long taskId, const std::vector<SegmentRow>& snapshot) {
    std::lock_guard<std::mutex> lk(mu_);
    segs_[taskId] = snapshot;
    return true;
}

bool MemoryTaskStore::removeSegments(long long taskId) {
    std::lock_guard<std::mutex> lk(mu_);
    segs_.erase(taskId);
    return true;
}

bool MemoryTaskStore::setTaskRunning(long long) { return true; }

bool MemoryTaskStore::setTaskTotalSize(long long, long long) { return true; }

bool MemoryTaskStore::setTaskTerminal(long long taskId, TaskStatus st, const std::string&,
                                      long long, int, const std::vector<TaskStatus>&) {
    if (st == TaskStatus::Success) {
        std::lock_guard<std::mutex> lk(mu_);
        segs_.erase(taskId);
    }
    return true;
}

} // namespace dlsvc
