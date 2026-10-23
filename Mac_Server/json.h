#pragma once
// json.h — 极简 JSON 解析/序列化（服务端自用，只覆盖所需语义）
// 动机：C_code 旧版用 strstr 抠 "content" 字段，\uXXXX 转义直接乱码。
// 这里实现完整字符串转义处理（含代理对），移植自同事 Python 版 json.loads 语义。
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;  // UTF-8
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    // 对象成员查找；不存在返回 nullptr
    const Value* get(const std::string& key) const;
    // 数组下标；越界返回 nullptr
    const Value* at(size_t i) const;
    // 字符串值；非字符串返回空串
    std::string as_string() const;
};

// 解析 JSON 文本；失败返回 false 并写 err（可为 nullptr）
bool parse(const std::string& text, Value& out, std::string* err = nullptr);

// 输出带引号的 JSON 字符串字面量（转义控制字符/引号/反斜杠，UTF-8 原样透传）
std::string escape(const std::string& s);

}  // namespace json
