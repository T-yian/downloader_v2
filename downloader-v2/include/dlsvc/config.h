#pragma once

// 服务配置：key=value 配置文件 + 内置默认值
// 见 config/download_server.conf.example 的键说明

#include <string>

#include "dlsvc/core/download_config.h"
#include "dlsvc/db/db_config.h"

namespace dlsvc {

struct ServiceConfig {
    // 接入层
    std::string listenHost = "0.0.0.0";
    int listenPort = 9090;

    // 存储
    std::string storageRoot = "./data"; // save_path 允许根目录（相对路径拼到它下面）
    bool restrictToRoot = true;         // 拒绝逃逸出根目录的 save_path（防目录穿越）

    // 调度层（D1 外层 / D7 两级限流）
    int workerCount = 4;                // 全局任务并发上限 = 线程池规模
    int queueCapacity = 1024;           // 阻塞队列容量，满 -> 提交 503
    int maxActivePerUser = 4;           // 单用户并发上限，超限 -> 提交 429

    // 进度节流（D2）
    long long flushIntervalMs = 2000;   // 节流窗口（1-5s）
    long long flushBytes = 8LL * 1024 * 1024; // 字节兜底阈值

    DownloadConfig download;
    DbConfig db;

    // 日志
    std::string logLevel = "info";
    std::string logFile;                // 空 = stderr

    bool loadFromFile(const std::string& path, std::string* err = nullptr);
};

} // namespace dlsvc
