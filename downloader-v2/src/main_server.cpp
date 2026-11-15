// 下载器 v2 服务入口：
//   配置 → 日志 → libcurl 全局初始化 → 连接池 → TaskManager(对账+workers)
//   → HttpApi → 信号驱动的优雅退出
//
// 优雅退出顺序：stop 接入层（不再收新请求）→ stop 调度层（关队列、请求取消、
// join workers、停 flusher）→ 连接池析构。全部协作式，不强杀。

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>

#include "dlsvc/api/http_api.h"
#include "dlsvc/config.h"
#include "dlsvc/core/http_client.h"
#include "dlsvc/log.h"
#include "dlsvc/scheduler/task_manager.h"

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop.store(true); }

void printUsage() {
    fprintf(stderr,
            "usage: download_server [-c config]\n"
            "  -c config   key=value 配置文件路径（缺省用内置默认值）\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string confPath;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            confPath = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printUsage();
            return 0;
        } else {
            printUsage();
            return 1;
        }
    }

    dlsvc::ServiceConfig cfg;
    if (!confPath.empty()) {
        std::string err;
        if (!cfg.loadFromFile(confPath, &err)) {
            fprintf(stderr, "load config failed: %s\n", err.c_str());
            return 1;
        }
    }

    dlsvc::LogLevel lv = dlsvc::LogLevel::Info;
    dlsvc::parseLogLevel(cfg.logLevel, lv);
    dlsvc::initLogger(lv, cfg.logFile);
    LOG_INFO("downloader v2 starting: port=%d workers=%d storage_root=%s", cfg.listenPort,
             cfg.workerCount, cfg.storageRoot.c_str());

    ::signal(SIGPIPE, SIG_IGN); // 写已关闭的 socket 不能靠默认行为杀进程
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);

    dlsvc::curlGlobalInitOnce();

    auto pool = std::make_unique<dlsvc::DbPool>(cfg.db, 4);
    {
        dlsvc::TaskManager tm(*pool, cfg);
        std::string err;
        if (!tm.start(&err)) {
            LOG_ERROR("task manager start failed: %s", err.c_str());
            return 1;
        }
        dlsvc::HttpApi api(tm, cfg);
        if (!api.start(&err)) {
            LOG_ERROR("http api start failed: %s", err.c_str());
            return 1;
        }
        LOG_INFO("ready: http://%s:%d  (POST /api/task/submit, GET /api/task/{id}, "
                 "POST /api/task/{id}/cancel)",
                 cfg.listenHost.c_str(), cfg.listenPort);

        while (!gStop.load()) ::usleep(200 * 1000);

        LOG_INFO("shutting down ...");
        api.stop();
        tm.stop();
    }
    pool.reset();
    LOG_INFO("bye");
    return 0;
}
