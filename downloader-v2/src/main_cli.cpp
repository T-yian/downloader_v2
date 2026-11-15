// dlcli：命令行直接驱动下载内核（无服务/无 DB）
//
// 用途：
//   1) 内核的独立验收工具——分片/续传/降级/取消，不依赖 MySQL 就能全跑通；
//   2) 排障——同一个 URL 在 CLI 路径下成功而服务路径失败，就能把问题圈在调度/DB 层。
//
// 账本用 MemoryTaskStore（TaskStore 接口的进程内实现）——内核逻辑一行不改。

#include <getopt.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "dlsvc/core/download_runner.h"
#include "dlsvc/core/memory_store.h"
#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace {

void printHelp() {
    fprintf(stderr,
            "usage: dlcli [-o out] [-s segments] [-r retries] <url>\n"
            "  -o, --out PATH      保存路径（默认 ./download.bin）\n"
            "  -s, --segments N    分片连接数上限（默认 4）\n"
            "  -r, --retries N     瞬时失败重试次数（默认 3）\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string url, out;
    int segments = 4;
    long long retries = 3;

    static option longopts[] = {
        {"out", required_argument, nullptr, 'o'},
        {"segments", required_argument, nullptr, 's'},
        {"retries", required_argument, nullptr, 'r'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "o:s:r:h", longopts, nullptr)) != -1) {
        switch (ch) {
            case 'o': out = optarg; break;
            case 's': segments = atoi(optarg); break;
            case 'r': retries = atoll(optarg); break;
            case 'h': printHelp(); return 0;
            default: printHelp(); return 1;
        }
    }
    if (optind < argc) url = argv[optind];
    if (url.empty()) {
        fprintf(stderr, "error: url required\n");
        printHelp();
        return 1;
    }
    if (out.empty()) out = "download.bin";
    if (!dlsvc::util::isHttpUrl(url)) {
        fprintf(stderr, "error: url must be http(s)\n");
        return 1;
    }

    dlsvc::initLogger(dlsvc::LogLevel::Warn);
    ::signal(SIGPIPE, SIG_IGN);

    auto ctx = std::make_shared<dlsvc::TaskContext>();
    ctx->id = 1;
    ctx->url = url;
    ctx->savePath = out;

    dlsvc::DownloadConfig dcfg;
    dcfg.maxSegmentsPerTask = segments;
    dcfg.maxRetries = retries;

    dlsvc::MemoryTaskStore store;
    dlsvc::DownloadRunner runner(dcfg, store);

    std::atomic<bool> done{false};
    dlsvc::DownloadOutcome outcome = dlsvc::DownloadOutcome::Failed;
    std::string failReason;
    const auto t0 = std::chrono::steady_clock::now();

    std::thread worker([&] {
        outcome = runner.run(ctx, failReason);
        done.store(true);
    });

    // 进度条：内核只更新 ctx 原子量，CLI 层自己轮询渲染
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const long long cur = ctx->downloadedBytes.load();
        const long long total = ctx->totalSize.load();
        if (total > 0) {
            fprintf(stderr, "\r%5.1f%%  %s / %s   ", 100.0 * cur / total,
                    dlsvc::util::humanBytes(cur).c_str(), dlsvc::util::humanBytes(total).c_str());
        } else {
            fprintf(stderr, "\r%s   ", dlsvc::util::humanBytes(cur).c_str());
        }
    }
    worker.join();
    fprintf(stderr, "\n");

    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const long long bytes = ctx->downloadedBytes.load();
    switch (outcome) {
        case dlsvc::DownloadOutcome::Success:
            printf("done: %s in %.1fs (avg %s/s) -> %s\n", dlsvc::util::humanBytes(bytes).c_str(),
                   dt,
                   dt > 0 ? dlsvc::util::humanBytes((long long)(bytes / dt)).c_str() : "-",
                   out.c_str());
            return 0;
        case dlsvc::DownloadOutcome::Canceled:
            printf("canceled at %s (resumable: rerun with same -o)\n",
                   dlsvc::util::humanBytes(bytes).c_str());
            return 2;
        case dlsvc::DownloadOutcome::Failed:
            fprintf(stderr, "failed: %s\n", failReason.c_str());
            return 1;
    }
    return 1;
}
