#!/bin/bash
# volvoice macOS 服务端一键编译打包（Apple clang + libcurl，无第三方依赖）
# 产物：volvoice-server（裸二进制）+ VolvoiceServer.app（菜单栏常驻版）
set -euo pipefail
cd "$(dirname "$0")"

SDK="$(xcrun --show-sdk-path)"

clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
  -isysroot "$SDK" \
  app_main.mm text_out.mm http_server.cpp asr.cpp http_client.cpp \
  config.cpp json.cpp log.cpp \
  -o volvoice-server \
  -lcurl -lpthread \
  -framework AppKit \
  -framework CoreGraphics \
  -framework ApplicationServices

# 打包 .app（LSUIElement：无 Dock 图标，只在菜单栏）
APP=VolvoiceServer.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp Info.plist "$APP/Contents/Info.plist"
cp volvoice-server "$APP/Contents/MacOS/volvoice-server"

# 稳定签名：辅助功能授权依赖签名身份；有开发证书时用证书（重编译不失效），否则 ad-hoc（每次重编需重新授权）
if security find-identity -v -p codesigning 2>/dev/null | grep -q '"Volvoice Dev Signing"'; then
  codesign --force --sign "Volvoice Dev Signing" "$APP"
else
  codesign --force -s - "$APP" 2>/dev/null || true
fi

echo "编译完成 → $(pwd)/volvoice-server"
echo "打包完成 → $(pwd)/$APP"
