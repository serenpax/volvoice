// main.cpp — vi PC_Server 托盘版入口
// 手机语音桥 PC 端：接收手机音频 → MiMo ASR 转写 → 文字上屏。
// 无控制台黑框：后台托盘常驻，单击托盘图标显示/隐藏日志窗口，
// 关闭窗口只是隐藏，右键托盘菜单退出才真正退出。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "asr.h"
#include "config.h"
#include "http_server.h"
#include "log.h"
#include "text_out.h"

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_LOGAPPEND = WM_APP + 2;
constexpr int IDM_SHOW = 100;
constexpr int IDM_EXIT = 101;

HINSTANCE g_hinst = nullptr;
HWND g_hwnd = nullptr;
HWND g_hedit = nullptr;
NOTIFYICONDATAW g_nid = {};
HttpServer* g_server = nullptr;
std::thread g_srv_thread;

std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
}

std::wstring lan_ipv4() {
    wchar_t ip[64] = L"";
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    char hostname[256] = "";
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
            sockaddr_in* addr = (sockaddr_in*)res->ai_addr;
            InetNtopW(AF_INET, &addr->sin_addr, ip, 64);
            freeaddrinfo(res);
        }
    }
    WSACleanup();
    return ip;
}

// 托盘小图标：绿底圆 + 白色麦克风（GDI 遮罩式图标）
HICON make_tray_icon() {
    const int W = 32, H = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = W;
    bi.bV5Height = -H;  // 自上而下
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, color);

    // 黑底（配合遮罩：圆外透明）
    RECT rc = {0, 0, W, H};
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(mem, &rc, black);

    // 绿色圆
    HBRUSH green = CreateSolidBrush(RGB(0x2E, 0x7D, 0x32));
    HGDIOBJ oldB = SelectObject(mem, green);
    HPEN noPen = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldP = SelectObject(mem, noPen);
    Ellipse(mem, 1, 1, W - 1, H - 1);

    // 白色麦克风（胶囊 + 立杆）
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    SelectObject(mem, white);
    RoundRect(mem, 13, 8, 19, 21, 6, 6);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(mem, pen);
    MoveToEx(mem, 10, 18, nullptr);
    LineTo(mem, 10, 20);
    Arc(mem, 10, 12, 22, 24, 10, 18, 22, 18);
    MoveToEx(mem, 16, 24, nullptr);
    LineTo(mem, 16, 27);
    MoveToEx(mem, 12, 27, nullptr);
    LineTo(mem, 20, 27);

    SelectObject(mem, oldB);
    SelectObject(mem, oldP);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(pen);

    // 遮罩：黑=不透明（圆内），白=透明（圆外）
    HBITMAP mask = CreateBitmap(W, H, 1, 1, nullptr);
    HDC maskDC = CreateCompatibleDC(nullptr);
    HGDIOBJ oldM = SelectObject(maskDC, mask);
    SelectObject(maskDC, black);
    RECT full = {0, 0, W, H};
    FillRect(maskDC, &full, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SelectObject(maskDC, green);
    HPEN noPen2 = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(maskDC, noPen2);
    Ellipse(maskDC, 1, 1, W - 1, H - 1);
    SelectObject(maskDC, oldM);
    DeleteDC(maskDC);
    DeleteObject(green);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void append_log(const std::string& line) {
    if (g_hwnd) {
        auto* p = new std::string(line);
        if (!PostMessageW(g_hwnd, WM_LOGAPPEND, 0, (LPARAM)p)) delete p;
    }
}

void show_window(bool show) {
    if (!g_hwnd) return;
    if (show) {
        ShowWindow(g_hwnd, SW_SHOW);
        ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void do_exit() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_server) g_server->stop();
    if (g_srv_thread.joinable()) g_srv_thread.join();
    DestroyWindow(g_hwnd);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            if (g_hedit) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                MoveWindow(g_hedit, 0, 0, rc.right - rc.left,
                           rc.bottom - rc.top, TRUE);
            }
            return 0;
        }
        case WM_LOGAPPEND: {
            auto* p = (std::string*)lp;
            std::wstring w = text_out::utf8_to_wide(*p);
            delete p;
            // 超长裁剪：只保留末尾约一半
            int len = GetWindowTextLengthW(g_hedit);
            if (len > 64000) {
                SendMessageW(g_hedit, EM_SETSEL, 0, len / 2);
                SendMessageW(g_hedit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            }
            int cur = GetWindowTextLengthW(g_hedit);
            SendMessageW(g_hedit, EM_SETSEL, cur, cur);
            w += L"\r\n";
            SendMessageW(g_hedit, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
            SendMessageW(g_hedit, EM_SCROLLCARET, 0, 0);
            return 0;
        }
        case WM_TRAYICON: {
            if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
                show_window(!IsWindowVisible(g_hwnd));
            } else if (lp == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, IDM_SHOW,
                            IsWindowVisible(g_hwnd) ? L"隐藏主窗口" : L"显示主窗口");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                               nullptr);
                DestroyMenu(menu);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wp) == IDM_SHOW) {
                show_window(!IsWindowVisible(g_hwnd));
            } else if (LOWORD(wp) == IDM_EXIT) {
                do_exit();
            }
            return 0;
        }
        case WM_CLOSE:
            // 关闭按钮 = 最小化到托盘，不是退出
            show_window(false);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    g_hinst = hinst;

    std::wstring config_path = exe_dir() + L"config.json";
    Config cfg;
    std::string err;
    if (!load_config(config_path, cfg, err)) {
        std::wstring msg = L"配置加载失败: " + text_out::utf8_to_wide(err) +
                           L"\n\n请检查配置文件: " + config_path;
        // 配置文件缺失时自动生成模板
        if (write_config_template(config_path)) {
            msg = L"未找到配置文件，已生成模板: " + config_path +
                  L"\n\n请编辑填入 api_base / api_key / token 后重新运行。";
        }
        MessageBoxW(nullptr, msg.c_str(), L"Volvoice Server", MB_OK | MB_ICONERROR);
        return 1;
    }
    slog::init(exe_dir() + cfg.log_file);
    slog::set_sink(append_log);

    // ---- 窗口 ----
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ViPcServerWnd";
    RegisterClassExW(&wc);

    std::wstring ip = lan_ipv4();
    std::wstring title = L"Volvoice Server — http://" +
                         (ip.empty() ? L"<本机IP>" : ip) + L":" +
                         std::to_wstring(cfg.port);
    g_hwnd = CreateWindowExW(0, L"ViPcServerWnd", title.c_str(),
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             680, 420, nullptr, nullptr, hinst, nullptr);
    g_hedit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 0, 0, g_hwnd, nullptr, hinst, nullptr);
    HFONT font = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, 0, 0, 0, 0, L"Consolas");
    SendMessageW(g_hedit, WM_SETFONT, (WPARAM)font, TRUE);

    // ---- 托盘图标 ----
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = make_tray_icon();
    lstrcpynW(g_nid.szTip, title.c_str(), 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // ---- 服务 ----
    slog::info("vi PC_Server（托盘版）启动中…");
    slog::info("ASR 网关: " + cfg.api_base + " / " + cfg.api_model);
    slog::info("鉴权 token 已加载（手机端需配置相同 token）");
    slog::info("手机端连接地址: http://" +
               text_out::wide_to_utf8(ip.empty() ? L"<本机IP>" : ip) + ":" +
               std::to_string(cfg.port));

    auto asr = std::make_unique<AsrClient>(cfg);
    auto server = std::make_unique<HttpServer>(cfg, *asr);
    g_server = server.get();

    // 预热网关连接（TLS 握手），降低第一句的转写延迟
    std::thread([a = asr.get()] {
        auto t0 = std::chrono::steady_clock::now();
        a->prewarm();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        slog::info("网关连接预热完成（" + std::to_string(ms) + "ms）");
    }).detach();

    if (!server->start(err)) {
        slog::error("服务启动失败: " + err);
        MessageBoxW(nullptr, text_out::utf8_to_wide(err).c_str(),
                    L"Volvoice Server", MB_OK | MB_ICONERROR);
        do_exit();
        return 1;
    }
    g_srv_thread = std::thread([s = server.get()] { s->run(); });

    // 初始隐藏在托盘（想看日志点托盘图标）
    show_window(false);

    // ---- 消息循环 ----
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    slog::info("退出");
    return 0;
}
