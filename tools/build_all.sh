#!/usr/bin/env bash
# 一键构建全部 WinCE ARM 工具（在 ENLYZE CeGCC 容器内执行）
set -euo pipefail

CEGCC_BIN="${CEGCC_BIN:-/opt/cegcc-arm/bin}"
CC="$CEGCC_BIN/arm-mingw32ce-gcc"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo ">> [1/3] probe.exe"
"$CC" -O2 -Wall -Wno-unused-function "$ROOT/probe/probe.c" \
  -o "$ROOT/probe/probe.exe" -static-libgcc

echo ">> [2/3] usbwatch.exe"
"$CC" -O2 -Wall -Wno-unused-function "$ROOT/usbwatch/usbwatch.c" \
  -o "$ROOT/usbwatch/usbwatch.exe" -static-libgcc

echo ">> [3/3] bench.exe"
"$CC" -O2 -Wall -Wno-unused-function "$ROOT/bench/bench.c" \
  -o "$ROOT/bench/bench.exe" -static-libgcc
# bench.c 内部 #include nanojpeg.c，无需单列源文件

for f in probe/probe.exe usbwatch/usbwatch.exe bench/bench.exe; do
  "$CEGCC_BIN/arm-mingw32ce-objdump" -f "$ROOT/$f" | grep -i "architecture: arm" \
    || { echo "ERROR: $f not ARM"; exit 1; }
  pe=$(od -An -tu4 -j60 -N4 "$ROOT/$f" | tr -d ' ')
  subs=$(od -An -tu2 -j$((pe + 92)) -N2 "$ROOT/$f" | tr -d ' ')
  [ "$subs" = "9" ] || { echo "ERROR: $f subsystem=$subs != 9"; exit 1; }
  echo "OK: $f (ARM, WinCE GUI)"
done
echo ">> all built"
