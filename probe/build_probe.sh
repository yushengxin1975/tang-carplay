#!/usr/bin/env bash
# 用 ENLYZE CeGCC（arm-mingw32ce, GCC 9.3）编译 Phase 0 探针为 ARM WinCE GUI 程序。
# 依赖：ghcr.io/enlyze/windows-ce-build-environment-arm 容器（或其中提取的 /opt/cegcc-arm）。
# 产物 probe.exe 自包含，拷到 U 盘即可上车运行。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CEGCC_BIN="${CEGCC_BIN:-/opt/cegcc-arm/bin}"
CC="$CEGCC_BIN/arm-mingw32ce-gcc"

echo ">> CC = $CC"
"$CC" -O2 -Wall -Wno-unused-function \
  "$SCRIPT_DIR/probe.c" \
  -o "$SCRIPT_DIR/probe.exe" \
  -static-libgcc
# 说明：
# - 不传 --subsystem,windowsce：工具链默认 PE Subsystem=9(WinCE GUI)、入口 WinMain（carlive 验证过）
# - 探针只依赖 coredll（默认链接），无 ws2/ddraw 等额外库

if command -v "$CEGCC_BIN/arm-mingw32ce-objdump" >/dev/null 2>&1; then
  "$CEGCC_BIN/arm-mingw32ce-objdump" -f "$SCRIPT_DIR/probe.exe" | grep -i arch
fi
echo ">> done: $SCRIPT_DIR/probe.exe"
