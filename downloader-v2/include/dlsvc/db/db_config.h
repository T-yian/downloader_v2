#pragma once

// MySQL 连接参数（独立头文件：config.h 是零依赖基础库，不能 include mysql.h）

#include <string>

namespace dlsvc {

struct DbConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "dlsvc";
    std::string password = "dlsvc123";
    std::string db = "downloader";
    int connectTimeoutS = 5;
};

} // namespace dlsvc
