#include "dlsvc/sha1.h"

#include <cstdio>
#include <cstring>

namespace dlsvc {

namespace {
inline uint32_t rol(uint32_t v, uint32_t b) { return (v << b) | (v >> (32 - b)); }

inline uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
} // namespace

Sha1::Sha1() { reset(); }

void Sha1::reset() {
    h0_ = 0x67452301;
    h1_ = 0xEFCDAB89;
    h2_ = 0x98BADCFE;
    h3_ = 0x10325476;
    h4_ = 0xC3D2E1F0;
    bits_ = 0;
    bufLen_ = 0;
    finalized_ = false;
}

void Sha1::update(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    bits_ += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - bufLen_;
        if (take > len) take = len;
        memcpy(buf_ + bufLen_, p, take);
        bufLen_ += take;
        p += take;
        len -= take;
        if (bufLen_ == 64) {
            transform(buf_);
            bufLen_ = 0;
        }
    }
}

void Sha1::transform(const unsigned char* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = be32(block + 4 * i);
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0_, b = h1_, c = h2_, d = h3_, e = h4_;
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = tmp;
    }
    h0_ += a;
    h1_ += b;
    h2_ += c;
    h3_ += d;
    h4_ += e;
}

std::string Sha1::finalHex() {
    if (finalized_) return "";
    finalized_ = true;

    const uint64_t bits = bits_;
    const unsigned char pad = 0x80;
    const unsigned char zero = 0;
    update(&pad, 1);
    while (bufLen_ != 56) update(&zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)((bits >> (56 - 8 * i)) & 0xFF);
    update(lenb, 8); // 注意：update 会继续累加 bits_，但这里已经不需要了

    char out[41];
    snprintf(out, sizeof(out), "%08x%08x%08x%08x%08x", h0_, h1_, h2_, h3_, h4_);
    return out;
}

std::string Sha1::hex(const void* data, size_t len) {
    Sha1 s;
    s.update(data, len);
    return s.finalHex();
}

std::string Sha1::hex(const std::string& s) { return hex(s.data(), s.size()); }

} // namespace dlsvc
