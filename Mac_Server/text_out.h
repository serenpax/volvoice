#pragma once
// text_out.h — 文本上屏（macOS 版）
//
//   paste()       = 剪贴板备份 → 写入 → ⌘V 注入 → 恢复剪贴板
//                   （需辅助功能权限，首次会弹系统授权提示）
//   clear_input() = AXUIElement ValuePattern 清空；失败回退 Ctrl+U/Ctrl+K
#include <string>

namespace text_out {

// 把文本粘贴到当前焦点窗口。strategy: "paste" 等，用于响应与日志。
bool paste(const std::string& text, std::string& strategy, std::string& err);

// 清空当前焦点输入框：
//   标准输入框 → AX ValueAttribute 置空
//   终端/TUI   → Ctrl+U 清光标前 + Ctrl+K 清光标后
bool clear_input(std::string& strategy, std::string& err);

}  // namespace text_out
