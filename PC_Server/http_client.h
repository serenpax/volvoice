#pragma once
// http_client.h — WinHTTP 客户端（连接常驻复用，对应同事 Python 版 AsrSession）
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include <string>

class HttpClient {
public:
    ~HttpClient() { close(); }

    // 建立/复用到 host:port 的连接；失败写 err
    bool open(const std::wstring& host, int port, bool https,
              int timeout_ms, std::string& err);
    void close();

    // POST body 到 path；headers 里带上鉴权等附加头。
    // 传输层失败返回 false 并写 err（连接会被重置，下次自动重连）；
    // HTTP 非 2xx 仍返回 true，status_out 供上层判断。
    bool post(const std::wstring& path, const std::string& body,
              const std::wstring& headers,
              int& status_out, std::string& resp_out, std::string& err);

private:
    HINTERNET session_ = nullptr;
    HINTERNET connect_ = nullptr;
    std::wstring host_;
    int port_ = 0;
    bool https_ = true;
    int timeout_ms_ = 30000;

    void reset_connection();
};
