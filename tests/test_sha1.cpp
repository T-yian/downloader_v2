// SHA-1 标准测试向量（FIPS 180-1 / 常用公开向量）+ 分次 update 一致性

#include "dlsvc/sha1.h"

#include <cstdio>
#include <string>

#include "check.h"

using dlsvc::Sha1;

int main() {
    CHECK(Sha1::hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(Sha1::hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(Sha1::hex("The quick brown fox jumps over the lazy dog") ==
          "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
    CHECK(Sha1::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // 分次 update 与一次性等价（跨块边界正确性）
    {
        Sha1 s1;
        s1.update("abc", 3);
        Sha1 s2;
        s2.update("a", 1);
        s2.update("bc", 2);
        CHECK(s1.finalHex() == s2.finalHex());
    }
    // >64 字节输入跨多块
    {
        std::string big(1000, 'x');
        Sha1 s;
        for (char c : big) s.update(&c, 1);
        CHECK(s.finalHex() == Sha1::hex(big));
    }

    printf("test_sha1: all pass\n");
    return 0;
}
