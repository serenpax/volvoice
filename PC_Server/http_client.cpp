// http_client.cpp — WinHTTP 实现
#include "http_client.h"

#include <winhttp.h>

#include <vector>

void HttpClient::reset_connection() {
    if (connect_) {
        WinHttpCloseHandle(connect_);
        connect_ = nullptr;
    }
}

void HttpClient::close() {
    reset_connection();
    if (session_) {
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
}

bool HttpClient::open(const std::wstring& host, int port, bool https,
                      int timeout_ms, std::string& err) {
    if (connect_ && host_ == host && port_ == port && https_ == https &&
        timeout_ms_ == timeout_ms) {
        return true;  // 连接可复用（keep-alive）
    }
    close();
    host_ = host;
    port_ = port;
    https_ = https;
    timeout_ms_ = timeout_ms;

    session_ = WinHttpOpen(L"vi-pc-server/1.0",
                           WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
        err = "WinHttpOpen 失败: " + std::to_string(GetLastError());
        return false;
    }
    int t = timeout_ms_;
    WinHttpSetTimeouts(session_, t, t, t, t);

    connect_ = WinHttpConnect(session_, host_.c_str(), (INTERNET_PORT)port_, 0);
    if (!connect_) {
        err = "WinHttpConnect 失败: " + std::to_string(GetLastError());
        close();
        return false;
    }
    return true;
}

bool HttpClient::post(const std::wstring& path, const std::string& body,
                      const std::wstring& headers,
                      int& status_out, std::string& resp_out, std::string& err) {
    status_out = 0;
    resp_out.clear();
    if (!connect_) {
        err = "连接未建立";
        return false;
    }

    DWORD flags = https_ ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connect_, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        err = "WinHttpOpenRequest 失败: " + std::to_string(GetLastError());
        reset_connection();
        return false;
    }

    int t = timeout_ms_;
    WinHttpSetTimeouts(request, t, t, t, t);

    BOOL ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1,
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        err = "HTTP 传输失败: " + std::to_string(GetLastError());
        WinHttpCloseHandle(request);
        reset_connection();  // keep-alive 可能已失效，重建（对应 AsrSession 的失败重连）
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    status_out = (int)status;

    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request, &avail)) break;
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read)) break;
        resp_out.append(buf.data(), read);
    }

    WinHttpCloseHandle(request);
    return true;
}
