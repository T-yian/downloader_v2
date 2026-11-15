// mini_json 测试：解析 / 转义 / unicode / dump 回读一致

#include "dlsvc/mini_json.h"

#include <cstdio>

#include "check.h"

using namespace mini;

int main() {
    // 1. 综合对象解析
    {
        std::string err;
        Json j;
        CHECK_MSG(Json::parse(R"({"url":"http://a/b?q=1","n":3.5,"i":42,"b":true,"neg":-7,"arr":[1,2,3],"s":"a\"b\\c\n中文"})", j, err), err.c_str());
        CHECK(j.isObject());
        const Json* url = j.find("url");
        CHECK(url && url->isString() && url->asString() == "http://a/b?q=1");
        CHECK(j.find("n") && j.find("n")->asNumber() == 3.5);
        CHECK(j.find("i") && j.find("i")->asNumber() == 42.0);
        CHECK(j.find("b") && j.find("b")->asBool() == true);
        CHECK(j.find("neg") && j.find("neg")->asNumber() == -7.0);
        CHECK(j.find("arr") && j.find("arr")->items().size() == 3);
        CHECK(j.find("s") && j.find("s")->asString() == "a\"b\\c\n中文");
        CHECK(j.find("missing") == nullptr);
    }

    // 2. \uXXXX 与代理对
    {
        Json j;
        std::string err;
        CHECK_MSG(Json::parse(R"({"a":"\u4e2d\u6587","b":"\ud83d\ude00"})", j, err), err.c_str());
        CHECK(j.find("a") && j.find("a")->asString() == "中文");      // U+4E2D U+6587
        CHECK(j.find("b") && j.find("b")->asString() == "😀");        // U+1F600 代理对
    }

    // 3. 非法输入必须被拒绝（接入层防线的第一环）
    {
        Json j;
        std::string err;
        CHECK(!Json::parse("{", j, err));
        CHECK(!Json::parse("{\"a\":}", j, err));
        CHECK(!Json::parse("null trailing", j, err));
        CHECK(!Json::parse("nul", j, err));
        CHECK(!Json::parse("\"unterminated", j, err));
        CHECK(!Json::parse("{\"a\":1}{", j, err));
    }

    // 4. dump → 回读一致（roundtrip）
    {
        Json j = Json::object();
        j["str"] = std::string("quote\" back\\ nl\n ctrl\x01");
        Json arr = Json::array();
        arr.push_back(Json((long long)1));
        arr.push_back(Json(true));
        j["arr"] = arr;
        std::string out = j.dump();
        Json back;
        std::string err;
        CHECK_MSG(Json::parse(out, back, err), err.c_str());
        CHECK(back.find("str") && back.find("str")->asString() == j.find("str")->asString());
        CHECK(back.find("arr") && back.find("arr")->items().size() == 2);
    }

    // 5. quoted()：手拼 JSON 响应时用
    {
        std::string q = Json::quoted("a\"b");
        CHECK(q == "\"a\\\"b\"");
    }

    // 6. Null 自动升级为对象/数组
    {
        Json j;
        CHECK(j.isNull());
        j["k"] = Json((long long)1);
        CHECK(j.isObject());
        Json a;
        a.push_back(Json((long long)1));
        CHECK(a.isArray());
    }

    printf("test_mini_json: all pass\n");
    return 0;
}
