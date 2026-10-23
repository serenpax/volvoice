#pragma once
// text_out.h — 文本上屏（移植同事 miremote actions.py 的上屏语义）
//
// 移植对照：
//   paste()   = type_text + set_clipboard_text/get_clipboard_text
//               （改进：粘贴后恢复剪贴板，不再污染用户剪贴板）
//   clear()   = clear_input（UIA ValuePattern 清空 / 终端 Ctrl+U+Ctrl+K）
//   按键注入  = actions.py _tap/_scan_code/send_combo（v2.2 扫描码注入，
//               修复扫描码敏感应用看不到键的问题）
#include <string>

namespace text_out {

// 把文本粘贴到当前焦点窗口（剪贴板备份 → 写入 → Ctrl+V → 恢复剪贴板）。
// strategy: "paste" / "paste(uia-fallback 后续)" 等，用于响应与日志。
bool paste(const std::wstring& text, std::string& strategy, std::string& err);

// 清空当前焦点输入框（同事版 clear_input 语义）：
//   标准输入框 → UIA ValuePattern SetValue("")
//   终端/TUI   → Ctrl+U 清光标前 + Ctrl+K 清光标后
bool clear_input(std::string& strategy, std::string& err);

// 宽字符 → UTF-8（响应 JSON 用）
std::string wide_to_utf8(const std::wstring& ws);
// UTF-8 → 宽字符（请求体解析用）
std::wstring utf8_to_wide(const std::string& s);

}  // namespace text_out
