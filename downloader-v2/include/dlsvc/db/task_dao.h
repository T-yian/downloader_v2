#pragma once

// task 表 DAO：任务登记 / 状态迁移 / 进度更新 / 对账与限流查询
//
// 两条贯穿全部写操作的纪律：
//   1) 所有字符串列一律过 sqlEscape（url/save_path 是外部输入，防注入是底线）
//   2) 状态迁移一律带条件 WHERE（防 cancel 与 worker 收尾的竞态）

#include <string>
#include <vector>

#include "dlsvc/core/types.h"
#include "dlsvc/db/db_pool.h"

namespace dlsvc {

class TaskDao {
public:
    explicit TaskDao(DbPool& pool) : pool_(pool) {}

    // 插入 pending 任务，回填 row.id
    bool create(TaskRow& row, std::string* err = nullptr);

    bool getById(long long id, TaskRow& out, std::string* err = nullptr);

    bool getByResumeKey(const std::string& key, TaskRow& out, bool& found);

    // 条件状态迁移：只有当前 status ∈ from 才更新；affected=0 表示状态已被别人改走
    // downloadedBytes / retryCount 传 -1 表示"不更新该列"
    bool updateStatusFrom(long long id, TaskStatus st, const std::string& failReason,
                          long long downloadedBytes, int retryCount,
                          const std::vector<TaskStatus>& from, long long& affected);

    // 进度节流写入（D2）；totalSize < 0 表示不更新
    bool updateProgress(long long id, long long downloadedBytes, long long totalSize);

    // 提交时队列满 → 回滚刚插入的记录
    bool deletePending(long long id);

    // D7 用户级限流：SELECT COUNT(*) 用 (user_id, status) 索引
    bool countActiveByUser(const std::string& userId, long long& out);

    // D5 重启对账：pending + downloading 全捞出来
    bool listForReconcile(std::vector<TaskRow>& out);

    // 管理接口：status / userId 传空串表示不过滤
    bool listTasks(const std::string& status, const std::string& userId, int limit,
                   std::vector<TaskRow>& out);

private:
    DbPool& pool_;
};

} // namespace dlsvc
