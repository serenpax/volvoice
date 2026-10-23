// http_server.cpp — HTTP 服务实现（每连接一线程，keep-alive 复用，工作串行化）
#include "http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "json.h"
#include "log.h"
#include "text_out.h"

namespace {

std::mutex g_work_mutex;  // ASR + 上屏串行（上屏目标是同一个焦点窗口）

const char* status_reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

bool send_all(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

void send_json(SOCKET s, int status, const std::string& body) {
    std::string head =
        std::string("HTTP/1.1 ") + std::to_string(status) + " " +
        status_reason(status) +
        "\r\nContent-Type: application/json; charset=utf-8"
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: keep-alive"
        "\r\nKeep-Alive: timeout=25"
        "\r\n\r\n";
    send_all(s, head);
    send_all(s, body);
}

std::string json_ok(const std::string& extra_fields) {
    return "{\"ok\":true" + extra_fields + "}";
}

std::string json_err(const std::string& msg) {
    return "{\"ok\":false,\"error\":" + json::escape(msg) + "}";
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

struct Request {
    std::string method, path;
    std::string body;
    std::string auth;         // Authorization 头原文
    std::string content_type;
    bool expect_continue = false;
    bool too_large = false;
    bool bad = false;
    bool want_close = false;
};

}  // namespace

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::string& err) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        err = "WSAStartup 失败";
        return false;
    }
    listen_ = (unsigned long long)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((SOCKET)listen_ == INVALID_SOCKET) {
        err = "socket 失败: " + std::to_string(WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt((SOCKET)listen_, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)cfg_.port);
    addr.sin_addr.s_addr =
        cfg_.bind_addr == "127.0.0.1" ? htonl(INADDR_LOOPBACK)
                                      : htonl(INADDR_ANY);
    if (bind((SOCKET)listen_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "bind 失败（端口 " + std::to_string(cfg_.port) +
              " 可能被占用）: " + std::to_string(WSAGetLastError());
        return false;
    }
    if (listen((SOCKET)listen_, 8) == SOCKET_ERROR) {
        err = "listen 失败: " + std::to_string(WSAGetLastError());
        return false;
    }
    running_ = true;
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if ((SOCKET)listen_ != INVALID_SOCKET) {
        closesocket((SOCKET)listen_);
        listen_ = (unsigned long long)INVALID_SOCKET;
        WSACleanup();
    }
}

void HttpServer::run() {
    slog::info("服务已启动，等待手机连接（端口 " + std::to_string(cfg_.port) + "）");
    while (running_) {
        sockaddr_in client = {};
        int clen = sizeof(client);
        SOCKET s = accept((SOCKET)listen_, (sockaddr*)&client, &clen);
        if (s == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        std::thread([this, s] { handle_client((unsigned long long)s); }).detach();
    }
    slog::info("服务已停止");
}

bool HttpServer::check_auth(const std::string& auth_header) const {
    // 接受 "Authorization: Bearer <token>" 或裸 token（X-Auth-Token 头同义）
    std::string expected = "Bearer " + cfg_.token;
    return auth_header == expected || auth_header == cfg_.token;
}

void HttpServer::handle_client(unsigned long long sock_u) {
    SOCKET s = (SOCKET)sock_u;

    // keep-alive：空闲 25 秒后断开
    DWORD recv_timeout = 25000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout,
               sizeof(recv_timeout));

    // 一条连接上串行服务多个请求（省掉每次 TCP/TLS 握手）
    for (int req_no = 0; req_no < 50 && running_; req_no++) {
        // 读请求头
        std::string head;
        char buf[8192];
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                closesocket(s);
                return;
            }
            head.append(buf, n);
            header_end = head.find("\r\n\r\n");
            if (head.size() > 65536) {
                send_json(s, 400, json_err("请求头过大"));
                closesocket(s);
                return;
            }
        }
        std::string header_part = head.substr(0, header_end + 2);
        std::string pre_body = head.substr(header_end + 4);

        // ---- 解析请求行与头部 ----
        Request req;
        long long content_length = 0;
        {
            size_t line_end = header_part.find("\r\n");
            std::string line = header_part.substr(0, line_end);
            size_t sp1 = line.find(' ');
            size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                req.bad = true;
            } else {
                req.method = line.substr(0, sp1);
                req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            size_t pos = line_end + 2;
            while (pos < header_part.size()) {
                size_t e = header_part.find("\r\n", pos);
                if (e == std::string::npos || e == pos) break;
                std::string h = header_part.substr(pos, e - pos);
                size_t colon = h.find(':');
                if (colon != std::string::npos) {
                    std::string key = lower(h.substr(0, colon));
                    std::string val = h.substr(colon + 1);
                    while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                        val.erase(0, 1);
                    if (key == "content-length") {
                        content_length = atoll(val.c_str());
                        if (content_length >
                            (long long)cfg_.max_body_mb * 1024 * 1024)
                            req.too_large = true;
                    } else if (key == "authorization") {
                        req.auth = val;
                    } else if (key == "x-auth-token") {
                        if (req.auth.empty()) req.auth = val;
                    } else if (key == "expect") {
                        if (lower(val).find("100-continue") != std::string::npos)
                            req.expect_continue = true;
                    } else if (key == "content-type") {
                        req.content_type = val;
                    } else if (key == "connection") {
                        if (lower(val).find("close") != std::string::npos)
                            req.want_close = true;
                    }
                }
                pos = e + 2;
            }

            // ---- 读 body ----
            if (req.expect_continue) {
                send_all(s, "HTTP/1.1 100 Continue\r\n\r\n");
            }
            req.body = pre_body;
            while ((long long)req.body.size() < content_length) {
                int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.body.append(buf, n);
                if (req.body.size() >
                    (size_t)cfg_.max_body_mb * 1024 * 1024)
                    break;
            }
        }

        if (req.bad) {
            send_json(s, 400, json_err("请求格式错误"));
            closesocket(s);
            return;
        }
        if (req.too_large) {
            send_json(s, 413, json_err("音频/文本超过大小上限"));
            closesocket(s);
            return;
        }

        // elapsed 从请求收完起算（keep-alive 空闲等待不计入）
        auto t0 = std::chrono::steady_clock::now();

        // ---- 路由 ----
        std::string resp_body;
        int status = 200;

        if (req.method == "GET" && req.path == "/ping") {
            resp_body =
                json_ok(",\"service\":\"vi-pc-server\",\"version\":\"1.1\"");

        } else if (req.method == "POST" && req.path == "/transcribe") {
            if (!check_auth(req.auth)) {
                status = 401;
                resp_body = json_err("鉴权失败：token 不匹配");
            } else {
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::string text, err;
                long long asr_ms = 0;
                if (!asr_.recognize_wav(req.body, text, err, &asr_ms)) {
                    status = 500;
                    resp_body = json_err(err);
                } else {
                    std::wstring wtext = text_out::utf8_to_wide(text);
                    std::string strategy, derr;
                    auto tp = std::chrono::steady_clock::now();
                    bool delivered = text_out::paste(wtext, strategy, derr);
                    long long paste_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - tp)
                            .count();
                    long long total_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
                    if (!delivered) {
                        status = 500;
                        resp_body = "{\"ok\":false,\"error\":" +
                                    json::escape("识别成功但上屏失败: " + derr) +
                                    ",\"text\":" + json::escape(text) + "}";
                    } else {
                        resp_body =
                            json_ok(",\"text\":" + json::escape(text) +
                                    ",\"strategy\":" + json::escape(strategy) +
                                    ",\"asr_ms\":" + std::to_string(asr_ms) +
                                    ",\"paste_ms\":" + std::to_string(paste_ms) +
                                    ",\"elapsed_ms\":" + std::to_string(total_ms));
                    }
                    slog::info("转写 asr=" + std::to_string(asr_ms) +
                               "ms paste=" + std::to_string(paste_ms) +
                               "ms: " + text.substr(0, 60));
                }
            }

        } else if (req.method == "POST" && req.path == "/text") {
            if (!check_auth(req.auth)) {
                status = 401;
                resp_body = json_err("鉴权失败：token 不匹配");
            } else {
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::wstring wtext = text_out::utf8_to_wide(req.body);
                std::string strategy, derr;
                if (!text_out::paste(wtext, strategy, derr)) {
                    status = 500;
                    resp_body = json_err("上屏失败: " + derr);
                } else {
                    resp_body = json_ok(",\"strategy\":" + json::escape(strategy));
                }
            }

        } else if (req.method == "POST" && req.path == "/clear") {
            if (!check_auth(req.auth)) {
                status = 401;
                resp_body = json_err("鉴权失败：token 不匹配");
            } else {
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::string strategy, derr;
                if (!text_out::clear_input(strategy, derr)) {
                    status = 500;
                    resp_body = json_err(derr);
                } else {
                    resp_body = json_ok(",\"strategy\":" + json::escape(strategy));
                }
            }

        } else {
            status = 404;
            resp_body = json_err("未知路由: " + req.method + " " + req.path);
        }

        send_json(s, status, resp_body);
        if (req.want_close) break;
    }
    closesocket(s);
}
