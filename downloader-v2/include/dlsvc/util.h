#pragma once

// 通用工具：字符串 / 路径 / 分片切分 / 断点匹配键

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dlsvc::util {

// 格式化（printf 风格），内部 4KB 截断
std::string stringPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string toLower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

bool parseLL(const std::string& s, long long& out);   // 全串必须是合法整数
bool parseBool(const std::string& s, bool& out);      // true/false/1/0
bool startsWith(const std::string& s, const char* prefix);

// 仅接受 http:// 或 https://
bool isHttpUrl(const std::string& url);

std::string dirName(const std::string& path);         // "/a/b/c" -> "/a/b"；无 '/' -> "."
std::string joinPath(const std::string& a, const std::string& b);

// save_path 归一化 + 安全校验：
//  1) 相对路径拼接 root，绝对路径原样；2) 词法消解 "." / ".."（越界直接拒绝）；
//  3) restrictToRoot=true 时结果必须落在 root 下（防目录穿越，接入层硬校验）
bool normalizeSavePath(const std::string& root, const std::string& raw, bool restrictToRoot,
                       std::string& out, std::string& err);

bool mkdirs(const std::string& path);                 // 递归 mkdir 0755
bool fileSize(const std::string& path, long long& out); // false: 不存在或不可访问
bool fileExists(const std::string& path);

std::string humanBytes(long long n);                  // "12.50 MB"

// 分片切分（闭区间 [start, end]）：
//  分片数 = clamp(total / minSegSize, 1, maxSegs)，均分，余数从前往后 +1
//  total <= 0 返回空（0 字节文件走空分片路径）
std::vector<std::pair<long long, long long>> splitSegments(long long total, int maxSegs,
                                                           long long minSegSize);

// 断点匹配键（D3）：sha1(url + 0x1f + save_path)
// 为什么不用 (url, save_path) 联合唯一索引：url 最长 2048，超出 InnoDB 索引长度限制，
// 降级成前缀索引会产生误碰撞；改为定长哈希列建唯一索引，冲突语义可控。
std::string makeResumeKey(const std::string& url, const std::string& savePath);

// "2026-11-14 20:30:12.345"（本地时间）
std::string nowStringMs();

} // namespace dlsvc::util
