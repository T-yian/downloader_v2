#pragma once

// libcurl 探测客户端 + 全局初始化

#include <curl/curl.h>
#include <string>

namespace dlsvc {

// curl_global_init 全局恰好一次（进程内任意线程都安全；必须在任何 easy 句柄前调用）
void curlGlobalInitOnce();

struct ProbeResult {
    bool ok = false;
    long httpCode = 0;
    long long contentLength = -1; // -1 = 未知（chunked / 服务器不给）
    bool acceptRanges = false;
    std::string error;
};

// 探测：先 HEAD 拿 Content-Length / Accept-Ranges；
// HEAD 被拒（405/501）或缺头时，用 Range: bytes=0-0 的 GET 兜底解析 Content-Range。
// 两次都不奏效 → ok=false（探测失败直接进 failed，fail_reason 里写明）。
class HttpClient {
public:
    explicit HttpClient(long connectTimeoutS) : connectTimeoutS_(connectTimeoutS) {}

    ProbeResult probe(const std::string& url);

private:
    long connectTimeoutS_;
};

// runner 与 probe 共用的 easy 句柄公共选项（跟随重定向/超时/低速判定/NOSIGNAL 等）
void curlSetCommon(CURL* h, const std::string& url, long connectTimeoutS,
                   const std::string& userAgent, long stallBps, long stallTimeS);

} // namespace dlsvc
