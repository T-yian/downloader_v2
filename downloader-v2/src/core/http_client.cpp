#include "dlsvc/core/http_client.h"

#include <curl/curl.h>
#include <cstring>
#include <mutex>

#include "dlsvc/log.h"
#include "dlsvc/util.h"

namespace dlsvc {

namespace {
std::once_flag gCurlInitFlag;
} // namespace

void curlGlobalInitOnce() {
    std::call_once(gCurlInitFlag, [] { curl_global_init(CURL_GLOBAL_ALL); });
}

// ---------------- 公共 easy 选项 ----------------

namespace {
struct HeaderSink {
    std::string raw;
};

size_t headerCb(char* buf, size_t sz, size_t n, void* ud) {
    auto* sink = static_cast<HeaderSink*>(ud);
    sink->raw.append(buf, sz * n);
    return sz * n;
}

size_t discardCb(char*, size_t sz, size_t n, void*) { return sz * n; }

struct EasyGuard {
    CURL* h = nullptr;
    EasyGuard() : h(curl_easy_init()) {}
    ~EasyGuard() {
        if (h) curl_easy_cleanup(h);
    }
};

// 在原始响应头里找 name: value（大小写不敏感，要求行首匹配）
bool headerValue(const std::string& raw, const char* name, std::string& value) {
    std::string lower = util::toLower(raw);
    std::string prefix = util::toLower(name);
    prefix += ":";
    size_t pos = 0;
    while (true) {
        size_t idx = lower.find(prefix, pos);
        if (idx == std::string::npos) return false;
        if (idx == 0 || lower[idx - 1] == '\n') {
            size_t start = idx + prefix.size();
            size_t stop = lower.find('\n', start);
            if (stop == std::string::npos) stop = raw.size();
            value = util::trim(raw.substr(start, stop - start));
            return true;
        }
        pos = idx + 1;
    }
}
} // namespace

void curlSetCommon(CURL* h, const std::string& url, long connectTimeoutS,
                   const std::string& userAgent, long stallBps, long stallTimeS) {
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, connectTimeoutS);
    // 多线程下必须关掉 curl 的信号超时机制（SIGALRM 会打到任意线程）
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, userAgent.c_str());
    // 低速看门狗：连续 stallTimeS 秒吞吐低于 stallBps 视为卡死，交给上层重试
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, stallBps);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, stallTimeS);
}

ProbeResult HttpClient::probe(const std::string& url) {
    curlGlobalInitOnce();
    ProbeResult out;

    // 第一轮：HEAD
    {
        EasyGuard eg;
        if (!eg.h) {
            out.error = "curl_easy_init failed";
            return out;
        }
        HeaderSink sink;
        curlSetCommon(eg.h, url, connectTimeoutS_, "dlsvc/2.0", 1024L, 30L);
        curl_easy_setopt(eg.h, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(eg.h, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(eg.h, CURLOPT_HEADERDATA, &sink);
        CURLcode rc = curl_easy_perform(eg.h);
        long code = 0;
        curl_easy_getinfo(eg.h, CURLINFO_RESPONSE_CODE, &code);
        out.httpCode = code;
        if (rc == CURLE_OK && code >= 200 && code < 300) {
            std::string v;
            long long cl = 0;
            if (headerValue(sink.raw, "Content-Length", v) && util::parseLL(v, cl))
                out.contentLength = cl;
            if (headerValue(sink.raw, "Accept-Ranges", v))
                out.acceptRanges = util::toLower(v).find("bytes") != std::string::npos;
        } else {
            out.error = util::stringPrintf("HEAD: rc=%d http=%ld", (int)rc, code);
        }
    }
    if (out.contentLength >= 0 && out.acceptRanges) {
        out.ok = true;
        return out;
    }

    // 第二轮：Range GET 兜底（HEAD 被拒 / Content-Length 缺失）
    {
        EasyGuard eg;
        if (!eg.h) {
            out.error = "curl_easy_init failed";
            return out;
        }
        HeaderSink sink;
        curlSetCommon(eg.h, url, connectTimeoutS_, "dlsvc/2.0", 1024L, 30L);
        curl_easy_setopt(eg.h, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(eg.h, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(eg.h, CURLOPT_HEADERDATA, &sink);
        curl_easy_setopt(eg.h, CURLOPT_WRITEFUNCTION, discardCb);
        CURLcode rc = curl_easy_perform(eg.h);
        long code = 0;
        curl_easy_getinfo(eg.h, CURLINFO_RESPONSE_CODE, &code);
        if (rc == CURLE_OK && code >= 200 && code < 300) {
            out.httpCode = code;
            out.error.clear();
            std::string cr;
            if (code == 206 && headerValue(sink.raw, "Content-Range", cr)) {
                // "bytes 0-0/12345" → total；"*/ *" 未知
                size_t slash = cr.rfind('/');
                out.acceptRanges = true;
                if (slash != std::string::npos && slash + 1 < cr.size() && cr[slash + 1] != '*') {
                    long long total = 0;
                    if (util::parseLL(cr.substr(slash + 1), total)) out.contentLength = total;
                }
            }
            if (out.contentLength < 0) {
                std::string v;
                long long cl = 0;
                if (headerValue(sink.raw, "Content-Length", v) && util::parseLL(v, cl))
                    out.contentLength = cl;
            }
            out.ok = true;
        } else if (out.error.empty()) {
            out.error = util::stringPrintf("RANGE-GET: rc=%d http=%ld", (int)rc, code);
        }
    }
    return out;
}

} // namespace dlsvc
