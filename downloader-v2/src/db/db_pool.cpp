#include "dlsvc/db/db_pool.h"

#include <chrono>
#include <cstring>

#include "dlsvc/log.h"

namespace dlsvc {

DbPool::DbPool(const DbConfig& cfg, size_t poolSize) : cfg_(cfg), poolSize_(poolSize) {
    static bool libInited = [] {
        mysql_library_init(0, nullptr, nullptr);
        return true;
    }();
    (void)libInited;
}

DbPool::~DbPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    cv_.notify_all();
    std::lock_guard<std::mutex> lk(mu_);
    for (MYSQL* c : idle_) mysql_close(c);
    idle_.clear();
    // 借出的连接由 Handle 析构负责 close（pool_ 已置空时直接 mysql_close）
}

DbPool::Handle::~Handle() {
    if (!conn_) return;
    if (pool_) pool_->releaseConn(conn_);
    else mysql_close(conn_);
    conn_ = nullptr;
}

MYSQL* DbPool::newConn(std::string* err) {
    MYSQL* m = mysql_init(nullptr);
    if (!m) {
        if (err) *err = "mysql_init failed (OOM)";
        return nullptr;
    }
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &cfg_.connectTimeoutS);
    const char* charset = "utf8mb4";
    mysql_options(m, MYSQL_SET_CHARSET_NAME, charset);
    bool reconnect = true;
    mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);

    if (!mysql_real_connect(m, cfg_.host.c_str(), cfg_.user.c_str(), cfg_.password.c_str(),
                            cfg_.db.c_str(), (unsigned int)cfg_.port, nullptr, 0)) {
        if (err) *err = std::string(mysql_error(m));
        mysql_close(m);
        return nullptr;
    }
    return m;
}

std::unique_ptr<DbPool::Handle> DbPool::acquire(std::string* err, int timeoutMs) {
    MYSQL* c = nullptr;
    {
        std::unique_lock<std::mutex> lk(mu_);
        bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                [this] { return closed_ || !idle_.empty() || total_ < poolSize_; });
        if (!got) {
            if (err) *err = "db pool busy (acquire timeout)";
            return nullptr;
        }
        if (closed_) {
            if (err) *err = "db pool closed";
            return nullptr;
        }
        if (!idle_.empty()) {
            c = idle_.front();
            idle_.pop_front();
        } else {
            ++total_; // 先占坑再创建，防止超卖
        }
    }

    if (!c) {
        c = newConn(err);
        if (!c) {
            std::lock_guard<std::mutex> lk(mu_);
            --total_;
            cv_.notify_one();
            return nullptr;
        }
    }

    // 借出前健康检查；坏连接关掉重连一次
    if (mysql_ping(c) != 0) {
        LOG_WARN("db conn ping failed: %s, reconnecting", mysql_error(c));
        mysql_close(c);
        c = newConn(err);
        if (!c) {
            std::lock_guard<std::mutex> lk(mu_);
            --total_;
            cv_.notify_one();
            return nullptr;
        }
    }
    return std::unique_ptr<Handle>(new Handle(this, c));
}

void DbPool::releaseConn(MYSQL* c) {
    if (!c) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_) {
        mysql_close(c);
        return;
    }
    idle_.push_back(c);
    cv_.notify_one();
}

std::string sqlEscape(MYSQL* m, const std::string& s) {
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long n = mysql_real_escape_string(m, &out[0], s.c_str(), (unsigned long)s.size());
    out.resize(n);
    return out;
}

bool sqlExec(MYSQL* m, const std::string& sql, std::string* err) {
    if (mysql_query(m, sql.c_str()) != 0) {
        if (err) *err = sqlErr(m);
        LOG_ERROR("sql failed: %s | %s", sqlErr(m).c_str(), sql.substr(0, 200).c_str());
        return false;
    }
    return true;
}

std::string sqlErr(MYSQL* m) {
    const char* e = mysql_error(m);
    return e && *e ? e : "unknown mysql error";
}

} // namespace dlsvc
