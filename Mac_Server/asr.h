#pragma once
// asr.h — MiMo ASR 网关客户端（请求构造 + 健壮响应解析）
#include <string>

#include "config.h"
#include "http_client.h"

class AsrClient {
public:
    explicit AsrClient(const Config& cfg) : cfg_(cfg) {}

    // wav_bytes：完整 WAV 文件字节；成功写 text_out（UTF-8），失败写 err_out。
    // asr_ms（可选）：网关往返耗时，用于分段计时。
    // 传输失败自动重试一次（重建连接），对应同事版的指数退避思想（这里只需单次重试）。
    bool recognize_wav(const std::string& wav_bytes,
                       std::string& text_out, std::string& err_out,
                       long long* asr_ms = nullptr);

    // 预热：发一段极短静音音频把 TLS/连接建起来（结果忽略），降低首句延迟。
    void prewarm();

private:
    Config cfg_;
    HttpClient http_;

    bool do_request(const std::string& body, int& status,
                    std::string& resp, std::string& err);
};
