#include "dlsvc/mini_json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mini {

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json& Json::operator[](const std::string& key) {
    if (type_ == Type::Null) type_ = Type::Object;
    return obj_[key];
}

const Json* Json::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    auto it = obj_.find(key);
    return it == obj_.end() ? nullptr : &it->second;
}

void Json::push_back(Json v) {
    if (type_ == Type::Null) type_ = Type::Array;
    arr_.push_back(std::move(v));
}

void Json::escapeString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

std::string Json::quoted(const std::string& s) {
    std::string o;
    escapeString(s, o);
    return o;
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            double d = num_;
            if (d == std::floor(d) && std::fabs(d) < 9.0e15) {
                char b[32];
                snprintf(b, sizeof(b), "%lld", (long long)d);
                out += b;
            } else {
                char b[40];
                snprintf(b, sizeof(b), "%.17g", d);
                out += b;
            }
            break;
        }
        case Type::String: escapeString(str_, out); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& v : arr_) {
                if (!first) out += ',';
                first = false;
                v.dumpTo(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) out += ',';
                first = false;
                escapeString(kv.first, out);
                out += ':';
                kv.second.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

// ---------------- 解析器（嵌套成员类，可访问私有字段） ----------------

namespace {
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}
} // namespace

class Json::Parser {
public:
    const char* p;
    const char* end;
    std::string err;

    bool fail(const std::string& m) {
        if (err.empty()) err = m;
        return false;
    }

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parseValue(Json& out) {
        skipWs();
        if (p >= end) return fail("unexpected end of input");
        char c = *p;
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out = Json(s);
            return true;
        }
        if (c == 't') return parseLiteral("true", Json(true), out);
        if (c == 'f') return parseLiteral("false", Json(false), out);
        if (c == 'n') return parseLiteral("null", Json(), out);
        return parseNumber(out);
    }

    bool parseLiteral(const char* lit, Json&& v, Json& out) {
        size_t n = strlen(lit);
        if ((size_t)(end - p) < n || memcmp(p, lit, n) != 0) return fail("bad literal");
        p += n;
        out = std::move(v);
        return true;
    }

    bool parseObject(Json& out) {
        ++p; // {
        out = Json::object();
        skipWs();
        if (p < end && *p == '}') {
            ++p;
            return true;
        }
        for (;;) {
            skipWs();
            if (p >= end || *p != '"') return fail("object key must be string");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (p >= end || *p != ':') return fail("expect ':'");
            ++p;
            Json val;
            if (!parseValue(val)) return false;
            out.obj_[key] = std::move(val);
            skipWs();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            return fail("expect ',' or '}'");
        }
    }

    bool parseArray(Json& out) {
        ++p; // [
        out = Json::array();
        skipWs();
        if (p < end && *p == ']') {
            ++p;
            return true;
        }
        for (;;) {
            Json val;
            if (!parseValue(val)) return false;
            out.arr_.push_back(std::move(val));
            skipWs();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            return fail("expect ',' or ']'");
        }
    }

    bool parseHex4(uint32_t& v) {
        if (end - p < 4) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char h = *p++;
            v <<= 4;
            if (h >= '0' && h <= '9') v |= (uint32_t)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (uint32_t)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (uint32_t)(h - 'A' + 10);
            else return fail("bad hex digit");
        }
        return true;
    }

    bool parseString(std::string& out) {
        ++p; // 开引号
        out.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') {
                ++p;
                return true;
            }
            if (c == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 高代理：必须紧跟 \uXXXX 低代理
                            if (p + 1 < end && p[0] == '\\' && p[1] == 'u') {
                                p += 2;
                                uint32_t lo;
                                if (!parseHex4(lo)) return false;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                cp = 0xFFFD;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            cp = 0xFFFD; // 孤立低代理
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape char");
                }
                continue;
            }
            if (c < 0x20) return fail("raw control char in string");
            out.push_back((char)c);
            ++p;
        }
        return fail("unterminated string");
    }

    bool parseNumber(Json& out) {
        const char* s = p;
        if (p < end && *p == '-') ++p;
        bool any = false;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                           *p == '+' || *p == '-')) {
            ++p;
            any = true;
        }
        if (!any) return fail("bad number");
        std::string tok(s, (size_t)(p - s));
        char* endp = nullptr;
        double d = strtod(tok.c_str(), &endp);
        if (!endp || *endp != '\0') return fail("bad number: " + tok);
        out = Json(d);
        return true;
    }
};

bool Json::parse(const std::string& text, Json& out, std::string& err) {
    Json::Parser ps{text.data(), text.data() + text.size(), ""};
    if (!ps.parseValue(out)) {
        err = ps.err;
        return false;
    }
    ps.skipWs();
    if (ps.p != ps.end) {
        err = "trailing characters after JSON value";
        return false;
    }
    return true;
}

} // namespace mini
