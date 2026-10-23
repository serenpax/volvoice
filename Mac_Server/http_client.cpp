// http_client.cpp — libcurl 实现（easy handle 常驻复用 = TCP keep-alive）
#include "http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <vector>

namespace {

struct WriteCtx {
    std::string* out;
};

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    ctx->out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_header(char* ptr, size_t size, size_t nmemb, void*) {
    return size * nmemb;  // 状态行由 CURLINFO_RESPONSE_CODE 读取，头忽略
}

}  // namespace

void HttpClient::reset() {
    if (easy_) {
        curl_easy_cleanup(static_cast<CURL*>(easy_));
        easy_ = nullptr;
    }
}

bool HttpClient::open(const std::string& host, int port, bool https,
                      int timeout_ms, std::string& err) {
    std::string url = std::string(https ? "https://" : "http://") + host;
    if (port != 80 && port != 443) url += ":" + std::to_string(port);
    if (easy_ && host_ == host && port_ == port && https_ == https &&
        timeout_ms_ == timeout_ms) {
        return true;  // 连接可复用（keep-alive）
    }
    reset();
    host_ = host;
    port_ = port;
    https_ = https;
    timeout_ms_ = timeout_ms;
    base_url_ = url;

    CURL* easy = curl_easy_init();
    if (!easy) {
        err = "curl_easy_init 失败";
        return false;
    }
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)timeout_ms_);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
                     (long)std::min(timeout_ms_, 10000));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    easy_ = easy;
    return true;
}

void HttpClient::close() { reset(); }

bool HttpClient::post(const std::string& path, const std::string& body,
                      const std::string& headers,
                      int& status_out, std::string& resp_out, std::string& err) {
    status_out = 0;
    resp_out.clear();
    if (!easy_) {
        err = "连接未建立";
        return false;
    }
    CURL* easy = static_cast<CURL*>(easy_);

    // 附加头：按行拆成 slist
    struct curl_slist* hdrs = nullptr;
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t e = headers.find("\r\n", pos);
        std::string line =
            e == std::string::npos ? headers.substr(pos)
                                   : headers.substr(pos, e - pos);
        if (!line.empty()) hdrs = curl_slist_append(hdrs, line.c_str());
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    WriteCtx ctx{&resp_out};
    curl_easy_setopt(easy, CURLOPT_URL, (base_url_ + path).c_str());
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_header);

    CURLcode rc = curl_easy_perform(easy);
    curl_slist_free_all(hdrs);

    if (rc != CURLE_OK) {
        err = std::string("HTTP 传输失败: ") + curl_easy_strerror(rc);
        reset();  // keep-alive 可能已失效，重建（对应 WinHTTP 版失败重连）
        return false;
    }

    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    status_out = (int)status;
    return true;
}
