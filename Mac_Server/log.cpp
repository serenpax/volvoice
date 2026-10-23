// log.cpp — 日志实现（macOS）
#include "log.h"

#include <cstdio>
#include <ctime>
#include <mutex>

namespace slog {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
Sink g_sink = nullptr;

std::string timestamp() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
             tmv.tm_min, tmv.tm_sec);
    return buf;
}

void write(const char* level, const std::string& msg) {
    std::string line = timestamp() + " [" + level + "] " + msg;
    Sink sink = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) {
            fwrite(line.data(), 1, line.size(), g_file);
            fputc('\n', g_file);
            fflush(g_file);
        }
        sink = g_sink;
    }
    if (sink) sink(line);
}
}  // namespace

void init(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    if (!file_path.empty()) {
        g_file = fopen(file_path.c_str(), "ab");
    }
}

void set_sink(Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = sink;
}

void info(const std::string& msg) { write("INFO", msg); }
void error(const std::string& msg) { write("ERR ", msg); }

}  // namespace slog
