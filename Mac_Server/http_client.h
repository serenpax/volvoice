#pragma once
// http_client.h — libcurl 客户端（连接常驻复用，对应 Windows 版 WinHTTP 实现）
#include <string>

class HttpClient {
public:
    ~HttpClient() { close(); }

    // 建立/复用到 host:port 的连接；失败写 err
    bool open(const std::string& host, int port, bool https,
              int timeout_ms, std::string& err);
    void close();

    // POST body 到 path；headers 为附加头（每行 "Key: Value\r\n"）。
    // 传输层失败返回 false 并写 err（连接会被重置，下次自动重连）；
    // HTTP 非 2xx 仍返回 true，status_out 供上层判断。
    bool post(const std::string& path, const std::string& body,
              const std::string& headers,
              int& status_out, std::string& resp_out, std::string& err);

private:
    void* easy_ = nullptr;   // CURL*（头文件不暴露 curl 类型）
    std::string base_url_;   // scheme://host:port
    std::string host_;
    int port_ = 0;
    bool https_ = true;
    int timeout_ms_ = 30000;

    void reset();
};
