// 工具函数测试：分片切分 / 路径归一化与穿越防护 / 其他小件

#include "dlsvc/util.h"

#include <cstdio>

#include "check.h"

using namespace dlsvc::util;

int main() {
    // 1. splitSegments：均分 + 余数前移
    {
        auto plan = splitSegments(10, 3, 1);
        CHECK(plan.size() == 3);
        CHECK(plan[0].first == 0 && plan[0].second == 3);  // len 4（余数给第一个）
        CHECK(plan[1].first == 4 && plan[1].second == 6);
        CHECK(plan[2].first == 7 && plan[2].second == 9);
    }
    {
        auto plan = splitSegments(100, 8, 8 * 1024 * 1024);
        CHECK(plan.size() == 1); // 小文件不分片
        CHECK(plan[0].first == 0 && plan[0].second == 99);
    }
    {
        auto plan = splitSegments(1024 * 1024 * 100, 8, 8 * 1024 * 1024); // 100MB / 8MB 下限 → 8 片
        CHECK(plan.size() == 8);
        long long covered = 0;
        for (auto& p : plan) covered += p.second - p.first + 1;
        CHECK(covered == 1024 * 1024 * 100); // 无缝全覆盖
        for (size_t i = 1; i < plan.size(); ++i) CHECK(plan[i].first == plan[i - 1].second + 1);
    }
    CHECK(splitSegments(0, 8, 1).empty());   // 0 字节 → 空计划
    CHECK(splitSegments(-5, 8, 1).empty());  // 非法 → 空计划

    // 2. normalizeSavePath：相对拼接 / 绝对放行 / 穿越拒绝
    {
        std::string out, err;
        CHECK_MSG(normalizeSavePath("/data/dlsvc", "models/weight.bin", true, out, err), err.c_str());
        CHECK(out == "/data/dlsvc/models/weight.bin");
    }
    {
        std::string out, err;
        CHECK_MSG(normalizeSavePath("/data/dlsvc", "/data/dlsvc/a.bin", true, out, err), err.c_str());
        CHECK(out == "/data/dlsvc/a.bin");
    }
    {
        std::string out, err;
        CHECK(!normalizeSavePath("/data/dlsvc", "/etc/passwd", true, out, err));
        CHECK(err.find("storage_root") != std::string::npos);
    }
    {
        std::string out, err;
        // 词法归一化后仍逃出根目录 → 拒
        CHECK(!normalizeSavePath("/data/dlsvc", "a/../../etc/passwd", true, out, err));
    }
    {
        std::string out, err;
        // 根目录内的 ".." 合法消解
        CHECK_MSG(normalizeSavePath("/data/dlsvc", "a/../b.bin", true, out, err), err.c_str());
        CHECK(out == "/data/dlsvc/b.bin");
    }
    {
        std::string out, err;
        CHECK(!normalizeSavePath("/data/dlsvc", "", true, out, err));
    }
    {
        std::string out, err;
        // restrict=false（CLI 模式）：root 只是拼接基准，只保证词法归一化成干净的
        // 绝对路径；根目录收口是 restrict=true（服务端）的职责，上面已单独验证
        CHECK_MSG(normalizeSavePath("/data/dlsvc", "/tmp/x.bin", false, out, err), err.c_str());
        CHECK(out == "/tmp/x.bin");
        // "a/../../x.bin" 词法归一化 = /data/x.bin（.. 只弹到 root 前一级）：
        // restrict=false 放行这个绝对路径；restrict=true 时前缀检查会拒绝它
        CHECK_MSG(normalizeSavePath("/data/dlsvc", "a/../../x.bin", false, out, err), err.c_str());
        CHECK(out == "/data/x.bin");
        // 但越过头部的 ".."（/../../x）在两种模式下都拒绝
        CHECK(!normalizeSavePath("/data/dlsvc", "/../../x.bin", false, out, err));
    }

    // 3. 杂项
    {
        CHECK(isHttpUrl("https://example.com/f.bin"));
        CHECK(isHttpUrl("http://example.com/f.bin"));
        CHECK(!isHttpUrl("ftp://example.com/f.bin"));
        CHECK(!isHttpUrl("httpx://a/b"));
        CHECK(!isHttpUrl(""));
    }
    {
        CHECK(humanBytes(0) == "0 B");
        CHECK(humanBytes(1024) == "1.00 KB");
        CHECK(humanBytes(5 * 1024 * 1024) == "5.00 MB");
    }
    {
        long long v = 0;
        CHECK(parseLL("123", v));
        CHECK(v == 123);
        CHECK(!parseLL("12x", v));
        CHECK(!parseLL("", v));
    }
    {
        CHECK(dirName("/a/b/c") == "/a/b");
        CHECK(dirName("/a") == "/");
        CHECK(dirName("file.bin") == ".");
    }

    // 4. 断点匹配键：确定性 + 分隔符防拼接歧义
    {
        std::string k1 = makeResumeKey("http://a/b", "/x/c");
        std::string k2 = makeResumeKey("http://a/b", "/x/c");
        CHECK(k1 == k2);
        CHECK(k1.size() == 40);
        CHECK(makeResumeKey("http://a/xb", "/x/c") != makeResumeKey("http://a/x", "b/x/c"));
    }

    printf("test_util: all pass\n");
    return 0;
}
