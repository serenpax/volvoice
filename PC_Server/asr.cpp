// asr.cpp — MiMo ASR 请求/响应实现
// 响应解析语义对齐同事 Python 版 parse_chat_asr_text：
// choices[0].message.content 为转写文本；错误取 error.message。
// 旧 C 版用 strstr 抠 "content"、不处理 \uXXXX——这里换成完整 JSON 解析。
#include "asr.h"

#include <chrono>
#include <cstdio>

#include "json.h"

namespace {

std::string b64_encode(const std::string& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        unsigned n = ((unsigned char)data[i] << 16) |
                     ((unsigned char)data[i + 1] << 8) |
                     (unsigned char)data[i + 2];
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += tbl[n & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        unsigned n = (unsigned char)data[i] << 16;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        unsigned n = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

}  // namespace

bool AsrClient::do_request(const std::string& body, int& status,
                           std::string& resp, std::string& err) {
    std::string err_open;
    if (!http_.open(cfg_.host, cfg_.host_port, cfg_.https,
                    cfg_.api_timeout_ms, err_open)) {
        err = err_open;
        return false;
    }

    std::wstring headers =
        L"Content-Type: application/json\r\n";
    if (cfg_.api_auth_style == "api-key") {
        // mify 网关风格（旧 C 版 config.h 的做法）
        headers += L"api-key: ";
    } else {
        // 标准 Bearer（同事版与 api.llm.mioffice.cn 网关的做法）
        headers += L"Authorization: Bearer ";
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, cfg_.api_key.c_str(),
                                  (int)cfg_.api_key.size(), nullptr, 0);
    std::wstring wkey(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cfg_.api_key.c_str(),
                        (int)cfg_.api_key.size(), &wkey[0], len);
    headers += wkey + L"\r\n";

    return http_.post(cfg_.req_path, body, headers, status, resp, err);
}

bool AsrClient::recognize_wav(const std::string& wav_bytes,
                              std::string& text_out, std::string& err_out,
                              long long* asr_ms) {
    auto t0 = std::chrono::steady_clock::now();
    auto mark = [&]() {
        if (asr_ms)
            *asr_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    };
    if (wav_bytes.empty()) {
        err_out = "音频数据为空";
        return false;
    }

    std::string b64 = b64_encode(wav_bytes);
    std::string body =
        "{\"model\":" + json::escape(cfg_.api_model) +
        ",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\","
        "\"input_audio\":{\"data\":\"data:audio/wav;base64," +
        b64 +
        "\"}}]}],\"asr_options\":{\"language\":" +
        json::escape(cfg_.api_language) + "}}";

    int status = 0;
    std::string resp, err;
    bool ok = do_request(body, status, resp, err);
    if (!ok) {
        // 传输失败：重建连接重试一次
        ok = do_request(body, status, resp, err);
        if (!ok) {
            mark();
            err_out = "ASR 请求失败: " + err;
            return false;
        }
    }
    mark();

    if (status < 200 || status >= 300) {
        err_out = "ASR 网关返回 HTTP " + std::to_string(status) +
                  ": " + resp.substr(0, 300);
        return false;
    }

    json::Value root;
    std::string perr;
    if (!json::parse(resp, root, &perr)) {
        err_out = "ASR 响应 JSON 解析失败: " + perr +
                  "; 原文: " + resp.substr(0, 200);
        return false;
    }

    // choices[0].message.content
    if (const json::Value* choices = root.get("choices")) {
        if (const json::Value* first = choices->at(0)) {
            if (const json::Value* msg = first->get("message")) {
                if (const json::Value* content = msg->get("content")) {
                    text_out = content->as_string();
                    return true;
                }
            }
        }
    }

    // 错误响应：error.message
    if (const json::Value* e = root.get("error")) {
        std::string msg;
        if (const json::Value* m = e->get("message")) msg = m->as_string();
        if (msg.empty()) msg = resp.substr(0, 200);
        err_out = "ASR 网关错误: " + msg;
        return false;
    }

    err_out = "ASR 响应缺少 choices[0].message.content: " + resp.substr(0, 200);
    return false;
}

void AsrClient::prewarm() {
    // 100ms 静音 WAV（16kHz 单声道 16bit = 3200 字节数据）
    const int data_len = 3200;
    std::string wav;
    wav.reserve(44 + data_len);
    auto put32 = [&](unsigned v) {
        for (int i = 0; i < 4; i++) wav += (char)((v >> (8 * i)) & 0xFF);
    };
    auto put16 = [&](unsigned v) {
        for (int i = 0; i < 2; i++) wav += (char)((v >> (8 * i)) & 0xFF);
    };
    wav += "RIFF";
    put32(36 + data_len);
    wav += "WAVEfmt ";
    put32(16);
    put16(1);            // PCM
    put16(1);            // mono
    put32(16000);        // sample rate
    put32(32000);        // byte rate
    put16(2);            // block align
    put16(16);           // bits
    wav += "data";
    put32(data_len);
    wav.append(data_len, '\0');

    std::string text, err;
    long long ms = 0;
    recognize_wav(wav, text, err, &ms);  // 结果忽略：只为把 TLS/连接建起来
}
