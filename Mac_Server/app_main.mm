// app_main.mm — volvoice macOS 菜单栏常驻版入口
// 对齐 Windows 托盘版体验：
//   - 菜单栏麦克风图标；单击显示/隐藏日志窗口，右键菜单退出
//   - 日志窗口标题 = 手机连接地址；点 ✕ 只隐藏，服务继续运行
//   - 配置/日志放 ~/Library/Application Support/Volvoice/
//   - 首次运行自动生成 config.json 模板
#import <AppKit/AppKit.h>

#include <libgen.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asr.h"
#include "config.h"
#include "http_server.h"
#include "log.h"

// ---------------- 工具 ----------------

namespace {

bool file_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string dir_of(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string exe_dir() {
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "./";
    char resolved[PATH_MAX];
    if (realpath(buf, resolved)) {
        std::string p(resolved);
        size_t slash = p.find_last_of('/');
        return slash == std::string::npos ? "./" : p.substr(0, slash + 1);
    }
    std::string p(buf);
    size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "./" : p.substr(0, slash + 1);
}

std::string app_support_dir() {
    const char* home = getenv("HOME");
    std::string dir =
        std::string(home ? home : "/tmp") + "/Library/Application Support/Volvoice";
    mkdir((std::string(home ? home : "/tmp") + "/Library").c_str(), 0755);
    mkdir((std::string(home ? home : "/tmp") + "/Library/Application Support").c_str(),
          0755);
    mkdir(dir.c_str(), 0755);
    return dir;
}

void copy_file(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

std::vector<std::string> lan_ipv4s() {
    std::vector<std::string> ips;
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return ips;
    for (struct ifaddrs* p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char ip[INET_ADDRSTRLEN] = "";
        auto* sin = (struct sockaddr_in*)p->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
            ips.push_back(ip);
    }
    freeifaddrs(ifap);
    return ips;
}

// 解析配置路径：优先 Application Support；兼容可执行文件旁的旧布局（存在则迁移）
std::string resolve_config_path(bool* created_template) {
    *created_template = false;
    std::string as_path = app_support_dir() + "/config.json";
    if (file_exists(as_path)) return as_path;
    std::string ex_path = exe_dir() + "config.json";
    if (file_exists(ex_path)) {
        copy_file(ex_path, as_path);
        return as_path;
    }
    if (write_config_template(as_path)) *created_template = true;
    return as_path;
}

std::unique_ptr<AsrClient> g_asr;
std::unique_ptr<HttpServer> g_server;
std::thread g_srv_thread;

void on_signal(int) {
    if (g_server) g_server->stop();
}

}  // namespace

// ---------------- AppDelegate ----------------

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow* logWindow;
@property(strong) NSTextView* logView;
@property(strong) NSStatusItem* statusItem;
@property(strong) NSMenu* statusMenu;
- (void)appendLog:(NSString*)line;
@end

static AppDelegate* g_delegate = nullptr;

namespace {
void sink_to_window(const std::string& line) {
    AppDelegate* d = g_delegate;
    if (!d) return;
    // 非法 UTF-8（如日志里被字节截断的中文）会让 stringWithUTF8String 返回 nil，
    // 进而使 appendAttributedString 抛异常导致进程 abort —— 必须回退到有损转换
    NSString* s = [[NSString alloc] initWithBytes:line.data()
                                           length:line.size()
                                         encoding:NSUTF8StringEncoding];
    if (!s) {
        s = [[NSString alloc] initWithBytes:line.data()
                                     length:line.size()
                                   encoding:NSISOLatin1StringEncoding];
    }
    if (!s) s = @"(无法解码的日志行)";
    NSString* out = [s stringByAppendingString:@"\n"];
    dispatch_async(dispatch_get_main_queue(), ^{
      [d appendLog:out];
    });
}
}  // namespace

@implementation AppDelegate

- (void)appendLog:(NSString*)line {
    if (!self.logView || line.length == 0) return;
    // 超长裁剪：只保留末尾约一半（对齐 Windows 版）
    if (self.logView.string.length > 64000) {
        NSString* keep = [self.logView.string
            substringFromIndex:self.logView.string.length / 2];
        [self.logView setString:keep];
    }
    NSDictionary* attrs = @{
        NSFontAttributeName : self.logView.font
            ?: [NSFont monospacedSystemFontOfSize:13
                                         weight:NSFontWeightRegular]
    };
    NSAttributedString* as = [[NSAttributedString alloc] initWithString:line
                                                            attributes:attrs];
    [self.logView.textStorage appendAttributedString:as];
    [self.logView
        scrollRangeToVisible:NSMakeRange(self.logView.string.length, 0)];
}

- (void)buildLogWindow {
    NSWindow* w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 680, 420)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    w.title = @"Volvoice Server";
    w.delegate = self;
    w.releasedWhenClosed = NO;

    NSScrollView* scroll = [[NSScrollView alloc]
        initWithFrame:w.contentView.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;

    NSTextView* tv = [[NSTextView alloc] initWithFrame:scroll.bounds];
    tv.editable = NO;
    tv.font = [NSFont monospacedSystemFontOfSize:13
                                         weight:NSFontWeightRegular];
    tv.autoresizingMask = NSViewWidthSizable;
    tv.textContainerInset = NSMakeSize(6, 6);
    scroll.documentView = tv;

    [w.contentView addSubview:scroll];
    [w center];

    self.logWindow = w;
    self.logView = tv;
}

- (void)buildStatusItem {
    self.statusItem =
        [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSImage* icon =
        [NSImage imageWithSystemSymbolName:@"mic.fill"
                      accessibilityDescription:@"Volvoice"];
    [icon setTemplate:YES];  // 自动适配深浅色菜单栏
    self.statusItem.button.image = icon;
    self.statusItem.button.toolTip = @"Volvoice Server";
    self.statusItem.button.target = self;
    self.statusItem.button.action = @selector(statusClicked:);
    [self.statusItem.button
        sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    [menu addItemWithTitle:@"显示 / 隐藏日志窗口"
                    action:@selector(toggleLogWindow)
             keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItemWithTitle:@"退出"
                    action:@selector(quit:)
             keyEquivalent:@"q"];
    self.statusMenu = menu;
}

- (void)statusClicked:(id)sender {
    if (NSApp.currentEvent.type == NSEventTypeRightMouseUp) {
        [self.statusMenu popUpMenuPositioningItem:nil
                                       atLocation:NSEvent.mouseLocation
                                           inView:nil];
    } else {
        [self toggleLogWindow];
    }
}

- (void)toggleLogWindow {
    if (self.logWindow.isVisible) {
        [self.logWindow orderOut:nil];
    } else {
        [self.logWindow makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

// 点 ✕ 只是隐藏到菜单栏，服务继续运行（对齐 Windows 版）
- (BOOL)windowShouldClose:(NSWindow*)sender {
    [sender orderOut:nil];
    return NO;
}

- (void)quit:(id)sender {
    [NSApp terminate:nil];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    if (g_server) g_server->stop();
    if (g_srv_thread.joinable()) g_srv_thread.join();
    slog::info("退出");
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    [self buildLogWindow];
    [self buildStatusItem];

    bool created_template = false;
    std::string cfg_path = resolve_config_path(&created_template);
    std::string cfg_dir = dir_of(cfg_path);

    Config cfg;
    std::string err;
    if (!load_config(cfg_path, cfg, err)) {
        slog::init(cfg_dir + "/" + cfg.log_file);
        slog::set_sink(sink_to_window);
        if (created_template) {
            slog::info("未找到配置文件，已生成模板: " + cfg_path);
            slog::info("请编辑填入 api_base / api_key 后重启本应用。");
            slog::info("（token 已自动生成，手机端设置里填一样的值即可）");
        } else {
            slog::info("配置加载失败: " + err);
            slog::info("请检查配置文件: " + cfg_path);
        }
        [self toggleLogWindow];
        return;
    }
    slog::init(cfg_dir + "/" + cfg.log_file);
    slog::set_sink(sink_to_window);

    slog::info("volvoice Mac 服务端（菜单栏版）启动中…");
    slog::info("配置文件: " + cfg_path);
    slog::info("ASR 网关: " + cfg.api_base + " / " + cfg.api_model);
    slog::info("鉴权 token 已加载（手机端需配置相同 token）");

    auto ips = lan_ipv4s();
    for (const auto& ip : ips)
        slog::info("手机端连接地址: http://" + ip + ":" +
                   std::to_string(cfg.port));
    if (!ips.empty())
        self.logWindow.title =
            [NSString stringWithFormat:@"Volvoice Server — http://%s:%d",
                                       ips[0].c_str(), cfg.port];

    g_asr = std::make_unique<AsrClient>(cfg);
    g_server = std::make_unique<HttpServer>(cfg, *g_asr);

    // 预热网关连接（TLS 握手），降低第一句的转写延迟
    std::thread([a = g_asr.get()] {
        auto t0 = std::chrono::steady_clock::now();
        a->prewarm();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        slog::info("网关连接预热完成（" + std::to_string(ms) + "ms）");
    }).detach();

    if (!g_server->start(err)) {
        slog::error("服务启动失败: " + err);
        [self toggleLogWindow];
        return;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_srv_thread = std::thread([] { g_server->run(); });
    slog::info("服务运行中 — 菜单栏麦克风图标：单击日志窗口，右键退出");
}

@end

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        AppDelegate* delegate = [AppDelegate new];
        g_delegate = delegate;
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
