#include "dlsvc/db/task_dao.h"

#include <cstdlib>
#include <cstring>

#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace dlsvc {

namespace {

// 显式列清单（不 SELECT *，schema 变更时语句不至于悄悄错位）
const char* kTaskCols =
    "id,user_id,url,save_path,resume_key,total_size,downloaded_bytes,status,fail_reason,"
    "retry_count,DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s.%f') AS created_at,"
    "DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i:%s.%f') AS updated_at";

TaskStatus statusOf(const char* s) {
    TaskStatus st = TaskStatus::Pending;
    if (s) parseStatus(s, st);
    return st;
}

void fillRow(MYSQL_ROW r, TaskRow& out) {
    #define S(i) (r[(i)] ? r[(i)] : "")
    out.id = strtoll(S(0), nullptr, 10);
    out.userId = S(1);
    out.url = S(2);
    out.savePath = S(3);
    out.resumeKey = S(4);
    out.totalSize = strtoll(S(5), nullptr, 10);
    out.downloadedBytes = strtoll(S(6), nullptr, 10);
    out.status = statusOf(S(7));
    out.failReason = S(8);
    out.retryCount = (int)strtol(S(9), nullptr, 10);
    out.createdAt = S(10);
    out.updatedAt = S(11);
    #undef S
}

// 把 from 状态集合拼成 SQL IN 列表："('pending','downloading')"
std::string statusInList(const std::vector<TaskStatus>& from) {
    std::string list = "(";
    for (size_t i = 0; i < from.size(); ++i) {
        if (i) list += ",";
        list += "'";
        list += toString(from[i]);
        list += "'";
    }
    list += ")";
    return list;
}

} // namespace

bool TaskDao::create(TaskRow& row, std::string* err) {
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf(
        "INSERT INTO task (user_id,url,save_path,resume_key,status,retry_count) "
        "VALUES ('%s','%s','%s','%s','%s',%d)",
        sqlEscape(m, row.userId).c_str(), sqlEscape(m, row.url).c_str(),
        sqlEscape(m, row.savePath).c_str(), sqlEscape(m, row.resumeKey).c_str(),
        sqlEscape(m, std::string(toString(row.status))).c_str(), row.retryCount);
    if (!sqlExec(m, sql, err)) return false;
    row.id = (long long)mysql_insert_id(m);
    return true;
}

bool TaskDao::getById(long long id, TaskRow& out, std::string* err) {
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf("SELECT %s FROM task WHERE id=%lld", kTaskCols, id);
    if (!sqlExec(m, sql, err)) return false;
    SqlResGuard guard(mysql_store_result(m));
    MYSQL_RES* res = guard.res;
    if (!res) {
        if (err) *err = sqlErr(m);
        return false;
    }
    MYSQL_ROW r = mysql_fetch_row(res);
    if (!r) return true; // 查不到不算错，out 保持默认（id=0）
    fillRow(r, out);
    return true;
}

bool TaskDao::getByResumeKey(const std::string& key, TaskRow& out, bool& found) {
    found = false;
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf("SELECT %s FROM task WHERE resume_key='%s' LIMIT 1",
                                         kTaskCols, sqlEscape(m, key).c_str());
    if (!sqlExec(m, sql)) return false;
    SqlResGuard guard(mysql_store_result(m));
    if (!guard.res) return false;
    MYSQL_ROW r = mysql_fetch_row(guard.res);
    if (r) {
        fillRow(r, out);
        found = true;
    }
    return true;
}

bool TaskDao::updateStatusFrom(long long id, TaskStatus st, const std::string& failReason,
                               long long downloadedBytes, int retryCount,
                               const std::vector<TaskStatus>& from, long long& affected) {
    affected = 0;
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf("UPDATE task SET status='%s',fail_reason='%s'",
                                         sqlEscape(m, std::string(toString(st))).c_str(),
                                         sqlEscape(m, failReason).c_str());
    if (downloadedBytes >= 0)
        sql += util::stringPrintf(",downloaded_bytes=%lld", downloadedBytes);
    if (retryCount >= 0)
        sql += util::stringPrintf(",retry_count=%d", retryCount);
    sql += util::stringPrintf(" WHERE id=%lld AND status IN %s", id, statusInList(from).c_str());
    if (!sqlExec(m, sql)) return false;
    affected = (long long)mysql_affected_rows(m);
    return true;
}

bool TaskDao::updateProgress(long long id, long long downloadedBytes, long long totalSize) {
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf("UPDATE task SET downloaded_bytes=%lld", downloadedBytes);
    if (totalSize >= 0) sql += util::stringPrintf(",total_size=%lld", totalSize);
    sql += util::stringPrintf(" WHERE id=%lld", id);
    return sqlExec(m, sql);
}

bool TaskDao::deletePending(long long id) {
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql =
        util::stringPrintf("DELETE FROM task WHERE id=%lld AND status='pending'", id);
    return sqlExec(m, sql);
}

bool TaskDao::countActiveByUser(const std::string& userId, long long& out) {
    out = 0;
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf(
        "SELECT COUNT(*) FROM task WHERE user_id='%s' AND status IN ('pending','downloading')",
        sqlEscape(m, userId).c_str());
    if (!sqlExec(m, sql)) return false;
    SqlResGuard guard(mysql_store_result(m));
    if (!guard.res) return false;
    MYSQL_ROW r = mysql_fetch_row(guard.res);
    out = r && r[0] ? strtoll(r[0], nullptr, 10) : 0;
    return true;
}

bool TaskDao::listForReconcile(std::vector<TaskRow>& out) {
    out.clear();
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf(
        "SELECT %s FROM task WHERE status IN ('pending','downloading') ORDER BY created_at, id",
        kTaskCols);
    if (!sqlExec(m, sql)) return false;
    SqlResGuard guard(mysql_store_result(m));
    if (!guard.res) return false;
    while (MYSQL_ROW r = mysql_fetch_row(guard.res)) {
        TaskRow row;
        fillRow(r, row);
        out.push_back(std::move(row));
    }
    return true;
}

bool TaskDao::listTasks(const std::string& status, const std::string& userId, int limit,
                        std::vector<TaskRow>& out) {
    out.clear();
    auto h = pool_.acquire();
    if (!h) return false;
    MYSQL* m = h->get();
    std::string where;
    if (!status.empty()) {
        TaskStatus st;
        if (!parseStatus(status, st)) return false; // 非法状态名直接拒绝
        where += "status='" + std::string(toString(st)) + "'";
    }
    if (!userId.empty()) {
        if (!where.empty()) where += " AND ";
        where += "user_id='" + sqlEscape(m, userId) + "'";
    }
    if (limit <= 0 || limit > 500) limit = 50;
    std::string sql = util::stringPrintf("SELECT %s FROM task %sORDER BY created_at DESC, id DESC LIMIT %d",
                                         kTaskCols, where.empty() ? "" : ("WHERE " + where + " ").c_str(),
                                         limit);
    if (!sqlExec(m, sql)) return false;
    SqlResGuard guard(mysql_store_result(m));
    if (!guard.res) return false;
    while (MYSQL_ROW r = mysql_fetch_row(guard.res)) {
        TaskRow row;
        fillRow(r, row);
        out.push_back(std::move(row));
    }
    return true;
}

} // namespace dlsvc
