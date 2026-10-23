// config.cpp — config.json 加载与 api_base URL 解析
#include "config.h"

#include <objbase.h>
#include <windows.h>

#include <cstdio>
#include <string>

#include "json.h"

namespace {

std::string read_file_utf8(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

bool parse_api_base(const std::string& base, Config& cfg, std::string& err) {
    std::string url = base;
    if (url.rfind("https://", 0) == 0) {
        cfg.https = true;
        cfg.host_port = 443;
        url = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        cfg.https = false;
        cfg.host_port = 80;
        url = url.substr(7);
    } else {
        err = "api_base 必须以 http:// 或 https:// 开头: " + base;
        return false;
    }
    std::string host_port, path;
    size_t slash = url.find('/');
    if (slash == std::string::npos) {
        host_port = url;
        path = "";
    } else {
        host_port = url.substr(0, slash);
        path = url.substr(slash);
    }
    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        cfg.host_port = atoi(host_port.substr(colon + 1).c_str());
        host_port = host_port.substr(0, colon);
    }
    if (host_port.empty()) {
        err = "api_base 缺少主机名: " + base;
        return false;
    }
    if (!path.empty() && path.back() == '/') path.pop_back();
    cfg.host = utf8_to_wide(host_port);
    cfg.req_path = utf8_to_wide(path + "/chat/completions");
    return true;
}

}  // namespace

bool load_config(const std::wstring& path, Config& out, std::string& err) {
    std::string text = read_file_utf8(path);
    if (text.empty()) {
        err = "无法读取配置文件";
        return false;
    }
    json::Value root;
    if (!json::parse(text, root, &err)) {
        err = "配置 JSON 解析失败: " + err;
        return false;
    }
    if (root.type != json::Value::Object) {
        err = "配置根节点必须是对象";
        return false;
    }

    auto str_of = [&](const char* key, std::string& dst) {
        if (const json::Value* v = root.get(key)) {
            if (v->type == json::Value::String) dst = v->str;
        }
    };
    auto int_of = [&](const char* key, int& dst) {
        if (const json::Value* v = root.get(key)) {
            if (v->type == json::Value::Number) dst = (int)v->num;
        }
    };

    int_of("port", out.port);
    str_of("bind", out.bind_addr);
    str_of("token", out.token);
    int_of("max_body_mb", out.max_body_mb);
    str_of("api_base", out.api_base);
    str_of("api_key", out.api_key);
    str_of("api_auth_style", out.api_auth_style);
    str_of("api_model", out.api_model);
    str_of("api_language", out.api_language);
    int_of("api_timeout_ms", out.api_timeout_ms);
    if (const json::Value* v = root.get("log_file")) {
        if (v->type == json::Value::String) out.log_file = utf8_to_wide(v->str);
    }

    if (out.token.empty()) {
        err = "配置缺少 token（手机端鉴权用，请设置一个随机长字符串）";
        return false;
    }
    if (out.api_key.empty()) {
        err = "配置缺少 api_key";
        return false;
    }
    if (out.api_key.find("在这里") != std::string::npos) {
        err = "api_key 还是模板占位符，请编辑 config.json 填入真实 key";
        return false;
    }
    return parse_api_base(out.api_base, out, err);
}

bool write_config_template(const std::wstring& path) {
    FILE* probe = _wfopen(path.c_str(), L"rb");
    if (probe) {
        fclose(probe);
        return false;  // 已存在，不动
    }

    // token：CoCreateGuid 的 16 字节转十六进制
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) return false;
    const unsigned char* raw = (const unsigned char*)&guid;
    char token[33];
    for (int i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", raw[i]);

    std::string content = std::string(R"({
  "_说明1": "vi PC_Server 配置。改完保存后重启 PC_Server.exe 生效。",
  "_说明2": "api_base: ASR 网关地址（OpenAI 风格，会自动拼 /chat/completions）。",
  "_说明3": "api_auth_style: bearer（Authorization: Bearer，api.llm.mioffice.cn）或 api-key（api-key 头，mify 网关）。",
  "_说明4": "token: 手机端鉴权串，手机 App 里必须填一样的值。api_key 仅存本机，请勿上传或提交。",
  "port": 30686,
  "bind": "0.0.0.0",
  "token": ")") + token + R"(",
  "max_body_mb": 25,
  "api_base": "https://api.llm.mioffice.cn/v1",
  "api_key": "在这里粘贴你的 API key",
  "api_auth_style": "bearer",
  "api_model": "xiaomi/mimo-v2.5-asr",
  "api_language": "auto",
  "api_timeout_ms": 30000,
  "log_file": "pc_server.log"
}
)";

    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return true;
}
