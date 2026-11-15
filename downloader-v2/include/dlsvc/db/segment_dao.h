#pragma once

// segment_progress 表 DAO：分片级断点账本（分片粒度决定续传起点精度）
// 主键 (task_id, seg_index)；批量 upsert 一条语句完成，减少事务数。

#include <string>
#include <vector>

#include "dlsvc/core/types.h"
#include "dlsvc/db/db_pool.h"

namespace dlsvc {

class SegmentDao {
public:
    explicit SegmentDao(DbPool& pool) : pool_(pool) {}

    // 计划整体替换（事务内 DELETE + 批量 INSERT）
    bool replaceAll(long long taskId, const std::vector<SegmentRow>& rows,
                    std::string* err = nullptr);

    bool loadByTask(long long taskId, std::vector<SegmentRow>& out, std::string* err = nullptr);

    // 进度落账：INSERT ... ON DUPLICATE KEY UPDATE，一条 SQL 带全部分片
    bool upsertProgress(long long taskId, const std::vector<SegmentRow>& rows,
                        std::string* err = nullptr);

    bool removeByTask(long long taskId, std::string* err = nullptr);

private:
    DbPool& pool_;
};

} // namespace dlsvc
