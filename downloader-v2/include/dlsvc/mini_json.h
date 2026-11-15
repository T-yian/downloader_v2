#pragma once

// 最小 JSON 库（手写，无第三方依赖）：
//   支持 null/bool/number/string/array/object，完整转义（含 \uXXXX 与代理对）。
//   服务化 MVP 够用；生产规模再换 nlohmann/json，接口形状刻意保持接近。
//
// 用法：
//   mini::Json j; std::string err;
//   if (mini::Json::parse(body, j, err)) { auto* u = j.find("url"); ... }
//   mini::Json r = mini::Json::object(); r["task_id"] = (long long)1;

#include <map>
#include <string>
#include <vector>

namespace mini {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(double d) : type_(Type::Number), num_(d) {}
    Json(int i) : type_(Type::Number), num_((double)i) {}
    Json(long long i) : type_(Type::Number), num_((double)i) {}
    Json(const char* s) : type_(Type::String), str_(s ? s : "") {}
    Json(const std::string& s) : type_(Type::String), str_(s) {}

    static Json array();
    static Json object();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0) const { return isNumber() ? num_ : def; }
    std::string asString(const std::string& def = "") const { return isString() ? str_ : def; }

    // 对象写（Null 自动升级为 Object）
    Json& operator[](const std::string& key);
    // 对象只读查找：不存在或非对象返回 nullptr
    const Json* find(const std::string& key) const;

    void push_back(Json v); // 数组追加（Null 自动升级为 Array）
    const std::vector<Json>& items() const { return arr_; }

    std::string dump() const;
    static bool parse(const std::string& text, Json& out, std::string& err);

    // "..."（带引号+转义）—— 手拼 JSON 响应时用
    static std::string quoted(const std::string& s);
    static void escapeString(const std::string& s, std::string& out);

private:
    // 解析器是 Json 的嵌套成员类（定义在 .cpp）：嵌套类天然可访问私有字段，
    // 又不污染头文件
    class Parser;

    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

} // namespace mini
