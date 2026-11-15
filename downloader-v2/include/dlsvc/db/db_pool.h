#pragma once

// MySQL 连接池：固定容量 + 空闲队列 + 借出健康检查。
//
// 为什么要有池：worker / flusher / API 三个来源都会并发写库，
// 每次操作现建连接（TCP+TLS+AUTH）延迟在毫秒级，高并发下不可接受；
// 池化后"借-用-还"，连接常驻，且借出前 mysql_ping 做活性检查 + 一次重连，
// DB 重启这类瞬时故障对上层透明。

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <mysql.h>

#include "dlsvc/db/db_config.h"

namespace dlsvc {

// mysql_free_result RAII 包装
struct SqlResGuard {
    MYSQL_RES* res = nullptr;
    explicit SqlResGuard(MYSQL_RES* r) : res(r) {}
    ~SqlResGuard() {
        if (res) mysql_free_result(res);
    }
    SqlResGuard(const SqlResGuard&) = delete;
    SqlResGuard& operator=(const SqlResGuard&) = delete;
};

class DbPool {
public:
    DbPool(const DbConfig& cfg, size_t poolSize);
    ~DbPool();

    class Handle {
    public:
        Handle(Handle&& o) noexcept : pool_(o.pool_), conn_(o.conn_) {
            o.pool_ = nullptr;
            o.conn_ = nullptr;
        }
        ~Handle();
        MYSQL* get() const { return conn_; }

    private:
        friend class DbPool;
        Handle(DbPool* p, MYSQL* c) : pool_(p), conn_(c) {}
        DbPool* pool_;
        MYSQL* conn_;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };

    // 借连接（带超时）；失败返回空指针，*err 描述原因
    std::unique_ptr<Handle> acquire(std::string* err = nullptr, int timeoutMs = 3000);

private:
    friend class Handle;
    MYSQL* newConn(std::string* err);
    void releaseConn(MYSQL* c);

    DbConfig cfg_;
    size_t poolSize_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<MYSQL*> idle_;
    size_t total_ = 0; // 已创建连接数（含借出）
    bool closed_ = false;
};

// ---- 通用 SQL 工具 ----
std::string sqlEscape(MYSQL* m, const std::string& s);
bool sqlExec(MYSQL* m, const std::string& sql, std::string* err = nullptr);
std::string sqlErr(MYSQL* m);

} // namespace dlsvc
