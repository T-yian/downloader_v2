#pragma once

// 下载内核参数（线程数 -> maxSegmentsPerTask 分片上限）

#include <cstdint>
#include <string>

namespace dlsvc {

struct DownloadConfig {
    int maxSegmentsPerTask = 8;                   // 单任务内部分片连接上限（D1 内层）
    long long minSegmentSize = 8LL * 1024 * 1024; // 文件小于该值不分片
    long connectTimeoutS = 10;
    long stallSpeedBps = 1024;                    // 低速卡死判定（看门狗雏形）
    long stallTimeS = 30;                         // 持续低速视为卡死
    long long maxRetries = 3;                     // 瞬时失败自动重试次数
    long long retryBackoffMs = 1000;              // 第 n 次退避 = n × 该值
    std::string userAgent = "dlsvc/2.0";
};

} // namespace dlsvc
