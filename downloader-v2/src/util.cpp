#include "dlsvc/util.h"

#include <cstdarg>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "dlsvc/sha1.h"

namespace dlsvc::util {

std::string stringPrintf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

std::string toLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t next = s.find(sep, pos);
        if (next == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

bool parseLL(const std::string& s, long long& out) {
    if (s.empty()) return false;
    errno = 0;
    char* endp = nullptr;
    long long v = strtoll(s.c_str(), &endp, 10);
    if (errno != 0 || endp == s.c_str() || *endp != '\0') return false;
    out = v;
    return true;
}

bool parseBool(const std::string& s, bool& out) {
    std::string v = toLower(trim(s));
    if (v == "true" || v == "1") { out = true; return true; }
    if (v == "false" || v == "0") { out = false; return true; }
    return false;
}

bool startsWith(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool isHttpUrl(const std::string& url) {
    return startsWith(url, "http://") || startsWith(url, "https://");
}

std::string dirName(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b[0] == '/') return b; // b 是绝对路径则直接用
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool normalizeSavePath(const std::string& root, const std::string& raw, bool restrictToRoot,
                       std::string& out, std::string& err) {
    if (raw.empty()) { err = "save_path empty"; return false; }
    if (raw.find('\0') != std::string::npos) { err = "save_path contains NUL"; return false; }
    if (raw.size() > 1024) { err = "save_path too long"; return false; }

    std::string candidate = raw;
    if (candidate[0] != '/') candidate = joinPath(root, candidate);

    // 词法归一化：不依赖磁盘上的 realpath（文件还不存在）
    std::vector<std::string> parts;
    for (const auto& seg : split(candidate, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (parts.empty()) { err = "save_path escapes above root"; return false; }
            parts.pop_back();
        } else {
            parts.push_back(seg);
        }
    }
    std::string norm = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        norm += parts[i];
        if (i + 1 < parts.size()) norm += "/";
    }

    if (restrictToRoot) {
        std::string r = root;
        while (r.size() > 1 && r.back() == '/') r.pop_back();
        if (r.empty()) r = "/";
        if (norm == r) { err = "save_path must be a file under storage_root"; return false; }
        if (norm.compare(0, r.size() + 1, r + "/") != 0) {
            err = "save_path must stay under storage_root: " + root;
            return false;
        }
    }
    out = norm;
    return true;
}

bool mkdirs(const std::string& path) {
    if (path.empty() || path == "/" || path == ".") return true;
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    if (errno == EEXIST) return true; // 简化：不校验是否为目录
    if (errno != ENOENT) return false;
    std::string parent = dirName(path);
    if (parent == path) return false;
    if (!mkdirs(parent)) return false;
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool fileSize(const std::string& path, long long& out) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return false;
    if (!S_ISREG(st.st_mode)) return false;
    out = (long long)st.st_size;
    return true;
}

bool fileExists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::string humanBytes(long long n) {
    if (n < 0) return "unknown";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0) return stringPrintf("%lld %s", n, units[u]);
    return stringPrintf("%.2f %s", v, units[u]);
}

std::vector<std::pair<long long, long long>> splitSegments(long long total, int maxSegs,
                                                           long long minSegSize) {
    std::vector<std::pair<long long, long long>> out;
    if (total <= 0 || maxSegs <= 0) return out; // 0 字节文件：空计划
    long long bySize = total / (minSegSize > 0 ? minSegSize : 1);
    int count = (int)std::max<long long>(1, std::min<long long>(bySize, maxSegs));
    long long base = total / count;
    long long rem = total % count;
    long long start = 0;
    for (int i = 0; i < count; ++i) {
        long long len = base + (i < rem ? 1 : 0);
        out.emplace_back(start, start + len - 1);
        start += len;
    }
    return out;
}

std::string makeResumeKey(const std::string& url, const std::string& savePath) {
    // 0x1f 分隔符防止 "ab"+"c" 与 "a"+"bc" 拼接歧义
    return Sha1::hex(url + "\x1f" + savePath);
}

std::string nowStringMs() {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    struct tm tmv{};
    localtime_r(&tv.tv_sec, &tmv);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(tv.tv_usec / 1000));
    return buf;
}

} // namespace dlsvc::util
