// text_out.mm — macOS 上屏实现：剪贴板备份/恢复 + ⌘V 注入 + AX 清空
#include "text_out.h"

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>

#include <string>
#include <unistd.h>

namespace text_out {

namespace {

// Virtual key codes（Events.h 的 kVK_*，避免引入已废弃的 Carbon 头）
constexpr unsigned short kVK_ANSI_V = 0x09;
constexpr unsigned short kVK_ANSI_U = 0x20;
constexpr unsigned short kVK_ANSI_K = 0x28;
constexpr unsigned short kVK_Command = 0x37;
constexpr unsigned short kVK_Control = 0x3B;

// 首次调用弹出系统授权提示，之后只做静默检查
bool ensure_trusted(std::string& err) {
    static bool prompted = false;
    if (AXIsProcessTrusted()) return true;
    if (!prompted) {
        prompted = true;
        NSDictionary* opts =
            @{(id)kAXTrustedCheckOptionPrompt : @YES};
        AXIsProcessTrustedWithOptions((CFDictionaryRef)opts);
        if (AXIsProcessTrusted()) return true;
    }
    err = "未获得辅助功能权限：请在 系统设置 → 隐私与安全性 → 辅助功能 中"
          "允许 VolvoiceServer（或运行本服务的终端），然后重试";
    return false;
}

// 向 HID 事件口注入"修饰键 + 主键"组合（模仿 Windows 版 send_combo）
bool post_combo(unsigned short mod, unsigned short key, int hold_ms = 30) {
    CGEventSourceRef src =
        CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    if (!src) return false;
    CGEventFlags mod_flag = (mod == kVK_Command) ? kCGEventFlagMaskCommand
                                                : kCGEventFlagMaskControl;

    auto make = [&](unsigned short vk, bool down) -> CGEventRef {
        CGEventRef ev = CGEventCreateKeyboardEvent(src, vk, down);
        if (ev) CGEventSetFlags(ev, CGEventGetFlags(ev) | mod_flag);
        return ev;
    };

    CGEventRef mod_down = make(mod, true);
    CGEventRef key_down = make(key, true);
    CGEventRef key_up = make(key, false);
    CGEventRef mod_up = make(mod, false);
    bool ok = mod_down && key_down && key_up && mod_up;
    if (ok) {
        CGEventPost(kCGHIDEventTap, mod_down);
        usleep(10000);
        CGEventPost(kCGHIDEventTap, key_down);
        usleep((useconds_t)(hold_ms * 1000));
        CGEventPost(kCGHIDEventTap, key_up);
        usleep(10000);
        CGEventPost(kCGHIDEventTap, mod_up);
    }
    if (mod_down) CFRelease(mod_down);
    if (key_down) CFRelease(key_down);
    if (key_up) CFRelease(key_up);
    if (mod_up) CFRelease(mod_up);
    CFRelease(src);
    return ok;
}

// AX 清空（对应 Windows 版 UIA ValuePattern 路径）
bool ax_clear(std::string& strategy) {
    AXUIElementRef sys = AXUIElementCreateSystemWide();
    CFTypeRef focused = nullptr;
    AXError e = AXUIElementCopyAttributeValue(
        sys, kAXFocusedUIElementAttribute, &focused);
    CFRelease(sys);
    if (e != kAXErrorSuccess || !focused) return false;

    bool ok = false;
    CFTypeRef val = nullptr;
    if (AXUIElementCopyAttributeValue((AXUIElementRef)focused,
                                      kAXValueAttribute, &val) ==
            kAXErrorSuccess &&
        val) {
        Boolean settable = false;
        AXUIElementIsAttributeSettable((AXUIElementRef)focused,
                                       kAXValueAttribute, &settable);
        if (settable) {
            CFStringRef empty = CFSTR("");
            if (AXUIElementSetAttributeValue((AXUIElementRef)focused,
                                             kAXValueAttribute,
                                             empty) == kAXErrorSuccess) {
                strategy = "clear(value)";
                ok = true;
            }
        }
        CFRelease(val);
    }
    CFRelease(focused);
    return ok;
}

}  // namespace

bool paste(const std::string& text, std::string& strategy, std::string& err) {
    if (text.empty()) {
        err = "文本为空";
        return false;
    }
    if (!ensure_trusted(err)) return false;

    @autoreleasepool {
        NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                                length:text.size()
                                              encoding:NSUTF8StringEncoding];
        if (!ns) {
            err = "文本不是合法 UTF-8";
            return false;
        }

        // 备份剪贴板（只备份文本；图片等其他格式无法恢复——与 Windows 版同限制）
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* prev = [pb stringForType:NSPasteboardTypeString];

        [pb clearContents];
        if (![pb setString:ns forType:NSPasteboardTypeString]) {
            err = "剪贴板写入失败";
            return false;
        }
        usleep(20000);  // 等剪贴板就绪

        if (!post_combo(kVK_Command, kVK_ANSI_V)) {
            err = "⌘V 注入失败（需要辅助功能权限）";
            if (prev) {
                [pb clearContents];
                [pb setString:prev forType:NSPasteboardTypeString];
            }
            return false;
        }
        usleep(60000);  // 给目标应用读取剪贴板留时间，再恢复

        if (prev) {
            [pb clearContents];
            [pb setString:prev forType:NSPasteboardTypeString];
        }
    }
    strategy = "paste";
    return true;
}

bool clear_input(std::string& strategy, std::string& err) {
    if (!ensure_trusted(err)) return false;
    if (ax_clear(strategy)) return true;

    // 终端/TUI 回退：Ctrl+U 删光标前 + Ctrl+K 删光标后（readline 语义）
    if (post_combo(kVK_Control, kVK_ANSI_U) &&
        post_combo(kVK_Control, kVK_ANSI_K)) {
        strategy = "clear(linekill)";
        return true;
    }
    err = "当前焦点控件不支持清空";
    return false;
}

}  // namespace text_out
