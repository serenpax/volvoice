// http_server.cpp — HTTP 服务实现（每连接一线程，keep-alive 复用，工作串行化）
#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
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

bool send_all(int s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(s, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void send_json(int s, int status, const std::string& body) {
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

// 按字节截断但保证落在 UTF-8 码点边界（防止截出非法序列）
std::string utf8_head(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t i = n;
    while (i > 0 && i < s.size() && (s[i] & 0xC0) == 0x80) i--;
    return s.substr(0, i);
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
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ < 0) {
        err = std::string("socket 失败: ") + strerror(errno);
        return false;
    }
    int reuse = 1;
    setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int nosigpipe = 1;
    setsockopt(listen_, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
               sizeof(nosigpipe));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg_.port);
    addr.sin_addr.s_addr =
        cfg_.bind_addr == "127.0.0.1" ? htonl(INADDR_LOOPBACK)
                                      : htonl(INADDR_ANY);
    if (bind(listen_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        err = "bind 失败（端口 " + std::to_string(cfg_.port) +
              " 可能被占用）: " + strerror(errno);
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    if (listen(listen_, 8) != 0) {
        err = std::string("listen 失败: ") + strerror(errno);
        ::close(listen_);
        listen_ = -1;
        return false;
    }
    running_ = true;
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (listen_ >= 0) {
        ::close(listen_);
        listen_ = -1;
    }
}

void HttpServer::run() {
    slog::info("服务已启动，等待手机连接（端口 " + std::to_string(cfg_.port) + "）");
    while (running_) {
        if (listen_ < 0) break;
        // select 超时轮询，保证 stop() 后 1 秒内退出
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_, &rfds);
        timeval tv = {1, 0};
        int r = select(listen_ + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0 || !running_) continue;

        sockaddr_in client = {};
        socklen_t clen = sizeof(client);
        int s = accept(listen_, (sockaddr*)&client, &clen);
        if (s < 0) {
            if (!running_) break;
            continue;
        }
        int nosigpipe = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
        std::thread([this, s] { handle_client(s); }).detach();
    }
    slog::info("服务已停止");
}

bool HttpServer::check_auth(const std::string& auth_header) const {
    // 接受 "Authorization: Bearer <token>" 或裸 token（X-Auth-Token 头同义）
    std::string expected = "Bearer " + cfg_.token;
    return auth_header == expected || auth_header == cfg_.token;
}

void HttpServer::handle_client(int s) {
    // keep-alive：空闲 25 秒后断开
    timeval recv_timeout = {25, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    // 一条连接上串行服务多个请求（省掉每次 TCP/TLS 握手）
    for (int req_no = 0; req_no < 50 && running_; req_no++) {
        // 读请求头
        std::string head;
        char buf[8192];
        size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            ssize_t n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(s);
                return;
            }
            head.append(buf, (size_t)n);
            header_end = head.find("\r\n\r\n");
            if (head.size() > 65536) {
                send_json(s, 400, json_err("请求头过大"));
                ::close(s);
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
                ssize_t n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.body.append(buf, (size_t)n);
                if (req.body.size() >
                    (size_t)cfg_.max_body_mb * 1024 * 1024)
                    break;
            }
        }

        if (req.bad) {
            send_json(s, 400, json_err("请求格式错误"));
            ::close(s);
            return;
        }
        if (req.too_large) {
            send_json(s, 413, json_err("音频/文本超过大小上限"));
            ::close(s);
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
                slog::error("鉴权失败（401）：/transcribe，token 不匹配");
            } else {
                slog::info("收到转写请求（" + std::to_string(req.body.size()) +
                           " 字节）");
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::string text, err;
                long long asr_ms = 0;
                if (!asr_.recognize_wav(req.body, text, err, &asr_ms)) {
                    status = 500;
                    resp_body = json_err(err);
                    slog::error("ASR 转写失败（" + std::to_string(asr_ms) +
                                "ms）: " + err);
                } else {
                    std::string strategy, derr;
                    auto tp = std::chrono::steady_clock::now();
                    bool delivered = text_out::paste(text, strategy, derr);
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
                        slog::error("上屏失败（paste=" +
                                    std::to_string(paste_ms) + "ms）: " + derr +
                                    "；转写文本: " + utf8_head(text, 60));
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
                               "ms: " + utf8_head(text, 60));
                }
            }

        } else if (req.method == "POST" && req.path == "/text") {
            if (!check_auth(req.auth)) {
                status = 401;
                resp_body = json_err("鉴权失败：token 不匹配");
                slog::error("鉴权失败（401）：/text，token 不匹配");
            } else {
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::string strategy, derr;
                if (!text_out::paste(req.body, strategy, derr)) {
                    status = 500;
                    resp_body = json_err("上屏失败: " + derr);
                    slog::error("上屏失败（/text）: " + derr);
                } else {
                    resp_body = json_ok(",\"strategy\":" + json::escape(strategy));
                }
            }

        } else if (req.method == "POST" && req.path == "/clear") {
            if (!check_auth(req.auth)) {
                status = 401;
                resp_body = json_err("鉴权失败：token 不匹配");
                slog::error("鉴权失败（401）：/clear，token 不匹配");
            } else {
                std::lock_guard<std::mutex> work(g_work_mutex);
                std::string strategy, derr;
                if (!text_out::clear_input(strategy, derr)) {
                    status = 500;
                    resp_body = json_err(derr);
                    slog::error("清空失败（/clear）: " + derr);
                } else {
                    resp_body = json_ok(",\"strategy\":" + json::escape(strategy));
                }
            }

        } else {
            status = 404;
            resp_body = json_err("未知路由: " + req.method + " " + req.path);
            slog::error("未知路由: " + req.method + " " + req.path);
        }

        send_json(s, status, resp_body);
        if (req.want_close) break;
    }
    ::close(s);
}
