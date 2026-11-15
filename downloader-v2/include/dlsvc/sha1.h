#pragma once

// 自带 SHA-1（无第三方依赖），只用于生成断点匹配键 resume_key，
// 不承担安全职责（url+save_path 非敏感），所以用 SHA-1 而不是 SHA-256 完全够用。

#include <cstddef>
#include <cstdint>
#include <string>

namespace dlsvc {

class Sha1 {
public:
    Sha1();

    void reset();
    void update(const void* data, size_t len);

    // 返回 40 位小写十六进制；一次对象只能 final 一次（final 后需 reset 复用）
    std::string finalHex();

    static std::string hex(const void* data, size_t len);
    static std::string hex(const std::string& s);

private:
    void transform(const unsigned char block[64]);

    uint32_t h0_, h1_, h2_, h3_, h4_;
    uint64_t bits_;
    unsigned char buf_[64];
    size_t bufLen_;
    bool finalized_ = false;
};

} // namespace dlsvc
