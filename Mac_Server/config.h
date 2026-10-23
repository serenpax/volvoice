#pragma once
// config.h — volvoice server 配置（config.json 加载）
#include <string>

struct Config {
    // 服务端
    int port = 30686;
    std::string bind_addr = "0.0.0.0";
    std::string token;                // 鉴权 token，手机端必须一致
    int max_body_mb = 25;

    // ASR 网关（由 api_base 解析出 host/port/path/https）
    std::string api_base = "https://api.llm.mioffice.cn/v1";
    std::string api_key;
    std::string api_auth_style = "bearer";  // bearer | api-key（mify 网关用 api-key）
    std::string api_model = "xiaomi/mimo-v2.5-asr";
    std::string api_language = "auto";
    int api_timeout_ms = 30000;

    std::string log_file = "volvoice-server.log";

    // api_base 解析结果
    bool https = true;
    std::string host;
    int host_port = 443;
    std::string req_path;  // 例如 /v1/chat/completions
};

// 从 JSON 文件加载配置；失败写 err。api_base 解析失败也报错。
bool load_config(const std::string& path, Config& out, std::string& err);

// 配置文件不存在时写一份模板（含自动生成的 token），返回是否已写入。
bool write_config_template(const std::string& path);
