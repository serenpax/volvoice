#pragma once
// http_server.h — 局域网 HTTP 服务（手机端接入点）
//
// 路由：
//   GET  /ping        无需鉴权，连通性测试
//   POST /transcribe  Bearer token；body=原始 WAV 字节 → ASR → 上屏
//   POST /text        Bearer token；body=UTF-8 纯文本 → 上屏
//   POST /clear       Bearer token；清空焦点输入框
#include <string>

#include "asr.h"
#include "config.h"

class HttpServer {
public:
    HttpServer(const Config& cfg, AsrClient& asr) : cfg_(cfg), asr_(asr) {}
    ~HttpServer();

    // 绑定 + 监听；失败写 err
    bool start(std::string& err);
    // 阻塞接受连接直到 stop()；每个连接独立线程处理
    void run();
    void stop();

private:
    Config cfg_;
    AsrClient& asr_;
    unsigned long long listen_ = ~0ull;  // SOCKET
    volatile bool running_ = false;

    void handle_client(unsigned long long sock);
    bool check_auth(const std::string& auth_header) const;
};
