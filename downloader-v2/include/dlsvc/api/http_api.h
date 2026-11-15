#pragma once

// 接入层（cpp-httplib）：把调度能力暴露成 HTTP API。
// 头文件用 Pimpl 隔离 httplib.h（1 万行的 header-only 库，只在 http_api.cpp
// 里编译一次，main_server.cpp 不再重复解析）。

#include <memory>
#include <string>

#include "dlsvc/config.h"
#include "dlsvc/scheduler/task_manager.h"

namespace dlsvc {

class HttpApi {
public:
    HttpApi(TaskManager& tm, const ServiceConfig& cfg);
    ~HttpApi();

    // 起 listen 线程并 wait_until_ready
    bool start(std::string* err = nullptr);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dlsvc
