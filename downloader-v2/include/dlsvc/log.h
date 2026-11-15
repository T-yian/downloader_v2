#pragma once

// 极简线程安全日志：时间戳 + 级别 + 内容（无第三方依赖）
// 默认写 stderr，可配置落文件；初始化在 main 里做一次。

#include <string>

namespace dlsvc {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// file 为空时输出到 stderr
void initLogger(LogLevel level, const std::string& file = "");

// "debug" / "info" / "warn" / "error"（大小写不敏感），失败返回 false 且 out 不变
bool parseLogLevel(const std::string& s, LogLevel& out);

void logWrite(LogLevel lv, const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 4, 5)));

} // namespace dlsvc

#define LOG_DEBUG(...) ::dlsvc::logWrite(::dlsvc::LogLevel::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  ::dlsvc::logWrite(::dlsvc::LogLevel::Info,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  ::dlsvc::logWrite(::dlsvc::LogLevel::Warn,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) ::dlsvc::logWrite(::dlsvc::LogLevel::Error, __FILE__, __LINE__, __VA_ARGS__)
