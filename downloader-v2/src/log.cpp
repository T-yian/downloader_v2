#include "dlsvc/log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "dlsvc/util.h"

namespace dlsvc {

namespace {
std::mutex gMu;
LogLevel gLevel = LogLevel::Info;
FILE* gFile = nullptr;
bool gOwnsFile = false; // 是否由我们 fopen（决定是否 fclose）

char levelTag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return 'D';
        case LogLevel::Info:  return 'I';
        case LogLevel::Warn:  return 'W';
        case LogLevel::Error: return 'E';
    }
    return '?';
}

const char* baseName(const char* path) {
    const char* p = strrchr(path, '/');
    return p ? p + 1 : path;
}
} // namespace

void initLogger(LogLevel level, const std::string& file) {
    std::lock_guard<std::mutex> g(gMu);
    gLevel = level;
    if (gOwnsFile && gFile) {
        fclose(gFile);
        gFile = nullptr;
        gOwnsFile = false;
    }
    if (!file.empty()) {
        gFile = fopen(file.c_str(), "a");
        gOwnsFile = gFile != nullptr;
        if (!gOwnsFile) fprintf(stderr, "[log] cannot open log file %s, fallback to stderr\n", file.c_str());
    }
}

bool parseLogLevel(const std::string& s, LogLevel& out) {
    std::string v = util::toLower(util::trim(s));
    if (v == "debug") { out = LogLevel::Debug; return true; }
    if (v == "info")  { out = LogLevel::Info;  return true; }
    if (v == "warn")  { out = LogLevel::Warn;  return true; }
    if (v == "error") { out = LogLevel::Error; return true; }
    return false;
}

void logWrite(LogLevel lv, const char* file, int line, const char* fmt, ...) {
    if (lv < gLevel) return;

    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    std::string ts = util::nowStringMs();
    std::lock_guard<std::mutex> g(gMu);
    FILE* out = gFile ? gFile : stderr;
    fprintf(out, "%s [%c] %s (%s:%d)\n", ts.c_str(), levelTag(lv), msg, baseName(file), line);
    if (gFile) fflush(gFile);
}

} // namespace dlsvc
