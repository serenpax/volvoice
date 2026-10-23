// json.cpp — 极简 JSON 解析/序列化实现
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

const Value* Value::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

const Value* Value::at(size_t i) const {
    if (type != Array || i >= arr.size()) return nullptr;
    return &arr[i];
}

std::string Value::as_string() const {
    return type == String ? str : std::string();
}

// ---------------- 解析 ----------------

namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string* err;

    void fail(const char* msg) {
        if (err && err->empty()) *err = msg;
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > 64) { fail("嵌套过深"); return false; }
        skip_ws();
        if (p >= end) { fail("意外结尾"); return false; }
        switch (*p) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"': out.type = Value::String; return parse_string(out.str);
            case 't':
                if (end - p >= 4 && strncmp(p, "true", 4) == 0) {
                    p += 4; out.type = Value::Bool; out.b = true; return true;
                }
                fail("非法字面量"); return false;
            case 'f':
                if (end - p >= 5 && strncmp(p, "false", 5) == 0) {
                    p += 5; out.type = Value::Bool; out.b = false; return true;
                }
                fail("非法字面量"); return false;
            case 'n':
                if (end - p >= 4 && strncmp(p, "null", 4) == 0) {
                    p += 4; out.type = Value::Null; return true;
                }
                fail("非法字面量"); return false;
            default: return parse_number(out);
        }
    }

    bool parse_number(Value& out) {
        char* num_end = nullptr;
        double v = strtod(p, &num_end);
        if (num_end == p || num_end > end) { fail("非法数字"); return false; }
        p = num_end;
        out.type = Value::Number;
        out.num = v;
        return true;
    }

    // 解析 4 位十六进制 → 码点
    bool parse_hex4(unsigned& cp) {
        if (end - p < 4) { fail("\\u 转义截断"); return false; }
        cp = 0;
        for (int i = 0; i < 4; i++) {
            char c = p[i];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
            else { fail("\\u 转义含非十六进制"); return false; }
        }
        p += 4;
        return true;
    }

    static void append_utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) {
            s += (char)cp;
        } else if (cp < 0x800) {
            s += (char)(0xC0 | (cp >> 6));
            s += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18));
            s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F));
            s += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parse_string(std::string& out) {
        if (p >= end || *p != '"') { fail("期待字符串"); return false; }
        p++;
        out.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p;
            if (c == '"') { p++; return true; }
            if (c != '\\') {
                out += (char)c;
                p++;
                continue;
            }
            p++;  // 跳过反斜杠
            if (p >= end) { fail("转义截断"); return false; }
            switch (*p) {
                case '"': out += '"'; p++; break;
                case '\\': out += '\\'; p++; break;
                case '/': out += '/'; p++; break;
                case 'b': out += '\b'; p++; break;
                case 'f': out += '\f'; p++; break;
                case 'n': out += '\n'; p++; break;
                case 'r': out += '\r'; p++; break;
                case 't': out += '\t'; p++; break;
                case 'u': {
                    p++;
                    unsigned cp = 0;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // 代理对：需要紧跟 \uXXXX 低位
                        if (end - p >= 2 && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo = 0;
                            if (!parse_hex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                fail("代理对低位非法"); return false;
                            }
                        } else {
                            fail("代理对缺失低位"); return false;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: fail("未知转义"); return false;
            }
        }
        fail("字符串未闭合");
        return false;
    }

    bool parse_array(Value& out, int depth) {
        p++;  // '['
        out.type = Value::Array;
        skip_ws();
        if (p < end && *p == ']') { p++; return true; }
        while (true) {
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            out.arr.push_back(std::move(v));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == ']') { p++; return true; }
            fail("数组语法错误");
            return false;
        }
    }

    bool parse_object(Value& out, int depth) {
        p++;  // '{'
        out.type = Value::Object;
        skip_ws();
        if (p < end && *p == '}') { p++; return true; }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') { fail("期待冒号"); return false; }
            p++;
            Value v;
            if (!parse_value(v, depth + 1)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (p < end && *p == ',') { p++; continue; }
            if (p < end && *p == '}') { p++; return true; }
            fail("对象语法错误");
            return false;
        }
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string* err) {
    if (err) err->clear();
    out = Value();
    Parser ps{text.data(), text.data() + text.size(), err};
    if (!ps.parse_value(out, 0)) return false;
    ps.skip_ws();
    if (ps.p != ps.end) {
        if (err) *err = "JSON 尾部有多余内容";
        return false;
    }
    return true;
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
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
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace json
