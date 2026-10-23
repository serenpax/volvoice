// text_out.h 实现 — 上屏：剪贴板备份/恢复粘贴 + UIA 清空
#include "text_out.h"

#include <windows.h>
#include <uiautomationclient.h>

#include <string>

namespace text_out {

// ---------------- 编码 ----------------

std::string wide_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], len,
                        nullptr, nullptr);
    return s;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                  nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

// ---------------- 按键注入（移植 actions.py _tap/_scan_code/send_combo） ----------------

namespace {

DWORD g_last_input_err = 0;

bool scan_code_of(WORD vk, WORD& scan, bool& extended) {
    // MAPVK_VK_TO_VSC_EX：返回 PS/2 Set-1 值，高字节 E0/E1 表示扩展键。
    // v2.2 实证：只填 VK 不填扫描码时事件 wScan=0，扫描码敏感应用完全看不到键。
    UINT sc = MapVirtualKeyW(vk, 4);
    if (sc == 0) {
        scan = 0;
        extended = false;
        return false;
    }
    scan = (WORD)(sc & 0xFF);
    extended = ((sc >> 8) == 0xE0 || (sc >> 8) == 0xE1);
    return true;
}

bool tap(WORD vk, bool up) {
    INPUT inp = {};
    inp.type = INPUT_KEYBOARD;
    inp.ki.wVk = vk;
    WORD scan = 0;
    bool extended = false;
    DWORD flags = up ? KEYEVENTF_KEYUP : 0;
    if (scan_code_of(vk, scan, extended)) {
        inp.ki.wScan = scan;
        flags |= KEYEVENTF_SCANCODE;
        if (extended) flags |= KEYEVENTF_EXTENDEDKEY;
    }
    inp.ki.dwFlags = flags;
    if (SendInput(1, &inp, sizeof(INPUT)) != 1) {
        g_last_input_err = GetLastError();
        return false;
    }
    return true;
}

// 按下所有键 → hold_last_ms → 逆序松开（同事版 send_combo 语义）
bool send_combo(const WORD* vks, size_t n, int hold_last_ms = 30) {
    if (n == 0) return false;
    bool ok = true;
    for (size_t i = 0; i < n; i++)
        if (!tap(vks[i], false)) ok = false;
    Sleep(hold_last_ms);
    for (size_t i = n; i > 0; i--)
        if (!tap(vks[i - 1], true)) ok = false;
    return ok;
}

// ---------------- 剪贴板（移植 set_clipboard_text/get_clipboard_text） ----------------

bool open_clipboard_retry() {
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(10);
    }
    return false;
}

bool clipboard_set_text(const std::wstring& text) {
    if (!open_clipboard_retry()) return false;
    bool ok = false;
    do {
        if (!EmptyClipboard()) break;
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!h) break;
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (!p) {
            GlobalFree(h);
            break;
        }
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) {
            GlobalFree(h);
            break;
        }
        ok = true;  // h 的所有权已交给系统
    } while (false);
    CloseClipboard();
    return ok;
}

// 有文本返回 true；无文本/失败返回 false（调用方据此决定是否恢复）
bool clipboard_get_text(std::wstring& out) {
    if (!open_clipboard_retry()) return false;
    bool ok = false;
    do {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (!h) break;
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (!p) break;
        out = p;
        GlobalUnlock(h);
        ok = true;
    } while (false);
    CloseClipboard();
    return ok;
}

// ---------------- UIA（移植 clear_input 的 ValuePattern 路径） ----------------

struct ComScope {
    bool uninit = false;
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit = (hr == S_OK || hr == S_FALSE);
    }
    ~ComScope() {
        if (uninit) CoUninitialize();
    }
};

bool uia_is_terminal_like(IUIAutomationElement* el) {
    BSTR cls = nullptr;
    if (FAILED(el->get_CurrentClassName(&cls)) || !cls) return false;
    std::wstring c(cls);
    SysFreeString(cls);
    return c.find(L"TermControl") != std::wstring::npos ||
           c.find(L"Console") != std::wstring::npos ||
           c.find(L"Terminal") != std::wstring::npos;
}

// UIA 清空（整个流程在单个 COM 作用域内完成，对象不出作用域）
bool uia_clear(std::string& strategy) {
    ComScope com;
    IUIAutomation* auto_ = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  (void**)&auto_);
    if (FAILED(hr) || !auto_) return false;

    IUIAutomationElement* el = nullptr;
    hr = auto_->GetFocusedElement(&el);
    if (FAILED(hr) || !el) {
        auto_->Release();
        return false;
    }

    // 1) 标准输入框：ValuePattern 一次 SetValue("")，无逐字动画
    // （MinGW 头文件签名是 GetCurrentPattern(PATTERNID, IUnknown**)）
    IUnknown* unk = nullptr;
    el->GetCurrentPattern(UIA_ValuePatternId, &unk);
    IUIAutomationValuePattern* vp = nullptr;
    if (unk) {
        unk->QueryInterface(IID_IUIAutomationValuePattern, (void**)&vp);
        unk->Release();
    }

    if (vp) {
        BOOL ro = FALSE;
        vp->get_CurrentIsReadOnly(&ro);
        HRESULT set_hr = E_FAIL;
        if (!ro) {
            BSTR empty = SysAllocString(L"");
            set_hr = vp->SetValue(empty);
            SysFreeString(empty);
        }
        if (SUCCEEDED(set_hr)) {
            strategy = "clear(value)";
            vp->Release();
            el->Release();
            auto_->Release();
            return true;
        }
    }

    // 2) 终端/TUI：Ctrl+U 删光标前 + Ctrl+K 删光标后
    if (uia_is_terminal_like(el)) {
        const WORD kill_line[] = {VK_CONTROL, 'U'};
        send_combo(kill_line, 2);
        Sleep(30);
        const WORD kill_rest[] = {VK_CONTROL, 'K'};
        send_combo(kill_rest, 2);
        strategy = "clear(linekill)";
        if (vp) vp->Release();
        el->Release();
        auto_->Release();
        return true;
    }

    if (vp) vp->Release();
    el->Release();
    auto_->Release();
    return false;
}

}  // namespace

// ---------------- 对外接口 ----------------

bool paste(const std::wstring& text, std::string& strategy, std::string& err) {
    if (text.empty()) {
        err = "文本为空";
        return false;
    }

    // 备份剪贴板（只备份文本；图片等其他格式无法恢复——同事版同样限制）
    std::wstring backup;
    bool had_backup = clipboard_get_text(backup);

    if (!clipboard_set_text(text)) {
        err = "剪贴板写入失败（可能被其他程序占用）";
        return false;
    }
    Sleep(20);  // 等剪贴板就绪（同事版 type_text 用 30ms，这里收紧）

    const WORD chord[] = {VK_CONTROL, 'V'};
    if (!send_combo(chord, 2)) {
        err = "Ctrl+V 注入失败，Win32 错误码 " + std::to_string(g_last_input_err) +
              "（5=UIPI 拒绝：目标窗口权限更高，或当前上下文禁止注入）";
        // 仍尝试恢复剪贴板
        if (had_backup) clipboard_set_text(backup);
        return false;
    }
    Sleep(60);  // 给目标应用读取剪贴板留时间，再恢复（原 80ms）
    if (had_backup) clipboard_set_text(backup);

    strategy = "paste";
    return true;
}

bool clear_input(std::string& strategy, std::string& err) {
    if (uia_clear(strategy)) return true;
    err = "当前焦点控件不支持清空";
    return false;
}

}  // namespace text_out
