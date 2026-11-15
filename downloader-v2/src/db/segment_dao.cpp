#include "dlsvc/db/segment_dao.h"

#include <cstdlib>

#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace dlsvc {

bool SegmentDao::replaceAll(long long taskId, const std::vector<SegmentRow>& rows,
                            std::string* err) {
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();

    if (!sqlExec(m, "START TRANSACTION", err)) return false;
    std::string del = util::stringPrintf("DELETE FROM segment_progress WHERE task_id=%lld", taskId);
    if (!sqlExec(m, del, err)) {
        sqlExec(m, "ROLLBACK");
        return false;
    }
    if (!rows.empty()) {
        // 批量 INSERT 一条语句完成；计划规模 = 分片数（≤ maxSegmentsPerTask），无超长风险
        std::string ins =
            "INSERT INTO segment_progress (task_id,seg_index,seg_start,seg_end,downloaded,finished) VALUES ";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) ins += ",";
            const SegmentRow& r = rows[i];
            ins += util::stringPrintf("(%lld,%d,%lld,%lld,%lld,%d)", taskId, r.segIndex, r.segStart,
                                      r.segEnd, r.downloaded, r.finished ? 1 : 0);
        }
        if (!sqlExec(m, ins, err)) {
            sqlExec(m, "ROLLBACK");
            return false;
        }
    }
    return sqlExec(m, "COMMIT", err);
}

bool SegmentDao::loadByTask(long long taskId, std::vector<SegmentRow>& out, std::string* err) {
    out.clear();
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf(
        "SELECT seg_index,seg_start,seg_end,downloaded,finished FROM segment_progress "
        "WHERE task_id=%lld ORDER BY seg_index",
        taskId);
    if (!sqlExec(m, sql, err)) return false;
    SqlResGuard guard(mysql_store_result(m));
    if (!guard.res) {
        if (err) *err = sqlErr(m);
        return false;
    }
    while (MYSQL_ROW r = mysql_fetch_row(guard.res)) {
        SegmentRow row;
        row.segIndex = r[0] ? atoi(r[0]) : 0;
        row.segStart = r[1] ? strtoll(r[1], nullptr, 10) : 0;
        row.segEnd = r[2] ? strtoll(r[2], nullptr, 10) : 0;
        row.downloaded = r[3] ? strtoll(r[3], nullptr, 10) : 0;
        row.finished = r[4] && atoi(r[4]) != 0;
        out.push_back(std::move(row));
    }
    return true;
}

bool SegmentDao::upsertProgress(long long taskId, const std::vector<SegmentRow>& rows,
                                std::string* err) {
    if (rows.empty()) return true;
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf(
        "INSERT INTO segment_progress (task_id,seg_index,seg_start,seg_end,downloaded,finished) "
        "VALUES ");
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) sql += ",";
        const SegmentRow& r = rows[i];
        sql += util::stringPrintf("(%lld,%d,%lld,%lld,%lld,%d)", taskId, r.segIndex, r.segStart,
                                  r.segEnd, r.downloaded, r.finished ? 1 : 0);
    }
    sql += " ON DUPLICATE KEY UPDATE seg_start=VALUES(seg_start),seg_end=VALUES(seg_end),"
           "downloaded=VALUES(downloaded),finished=VALUES(finished)";
    return sqlExec(m, sql, err);
}

bool SegmentDao::removeByTask(long long taskId, std::string* err) {
    auto h = pool_.acquire(err);
    if (!h) return false;
    MYSQL* m = h->get();
    std::string sql = util::stringPrintf("DELETE FROM segment_progress WHERE task_id=%lld", taskId);
    return sqlExec(m, sql, err);
}

} // namespace dlsvc
