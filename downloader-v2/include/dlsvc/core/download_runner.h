#pragma once

// 下载内核：
//
//   探测(HEAD/Range) → 计划分片 → 预分配 .dltmp → 分片并发 pwrite（D1 内层）
//     → 断点账本经 TaskStore 落库（D3）
//     → 字节数校验 → fsync + rename 原子发布
//
// 容错降级：不支持 Range / 总长未知 → 单流顺序下载，按文件大小续传；
// 瞬时失败（网络/5xx/低速卡死）自动重试，4xx（除 408/429）不重试直接判死；
// 取消（D6，协作式）：每个写盘块的开头检查 cancelRequested，命中即退出；
//   被取消的任务保留 .dltmp 与分片账本 → 同 url+save_path 再次提交即断点续传。

#include <memory>
#include <string>

#include "dlsvc/core/download_config.h"
#include "dlsvc/core/http_client.h"
#include "dlsvc/core/task_store.h"

namespace dlsvc {

enum class DownloadOutcome { Success, Canceled, Failed };

inline const char* toString(DownloadOutcome o) {
    switch (o) {
        case DownloadOutcome::Success: return "success";
        case DownloadOutcome::Canceled: return "canceled";
        case DownloadOutcome::Failed: return "failed";
    }
    return "unknown";
}

class DownloadRunner {
public:
    DownloadRunner(DownloadConfig cfg, TaskStore& store);

    // 阻塞执行整个下载（含探测/重试/落账）；failReason 仅在 Failed 时有意义
    DownloadOutcome run(TaskContextPtr ctx, std::string& failReason);

private:
    struct SegJob {
        int fd;
        TaskContext* ctx;
        SegmentProgress* seg;
        std::string error; // pwrite 层错误（写回调里捕获）
    };

    bool planAndPrepare(TaskContext& ctx, const ProbeResult& pr, bool degraded, int& outFd,
                        std::string& err);
    bool runLoop(TaskContext& ctx, int fd, bool degraded, std::string& err);
    bool runSegmentPass(TaskContext& ctx, int fd, bool degraded, std::string& err,
                        bool& permanentFail);
    std::string downloadOne(TaskContext& ctx, int fd, SegmentProgress& seg, bool sendRange,
                            bool& permanent);
    bool finalize(TaskContext& ctx, int fd, std::string& err);
    bool backoffSleep(TaskContext& ctx, long long ms);

    static size_t writeCb(char* data, size_t size, size_t n, void* ud);

    DownloadConfig cfg_;
    TaskStore& store_;
};

} // namespace dlsvc
