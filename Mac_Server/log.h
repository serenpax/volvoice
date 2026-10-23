#pragma once
// log.h — 文件日志（UTF-8）
#include <string>

namespace slog {

using Sink = void (*)(const std::string& line);

void init(const std::string& file_path);  // 空路径表示不写文件
void set_sink(Sink sink);                 // GUI 层注册日志接收器
void info(const std::string& msg);
void error(const std::string& msg);

}  // namespace slog
