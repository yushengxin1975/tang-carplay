#!/usr/bin/env bash
# Build Carlinkit CE client (Phase B.1 bring-up) using CeGCC.
# Output: client/carlinkit_ce.exe (ARM WinCE GUI).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CC="${CEGCC_BIN:-${CCGCC_BIN:-/opt/cegcc-arm/bin}}/arm-mingw32ce-g++"
STRIP="${CEGCC_BIN:-${CCGCC_BIN:-/opt/cegcc-arm/bin}}/arm-mingw32ce-strip"

if [ ! -x "$CC" ]; then
  echo "FATAL: CeGCC compiler not found at $CC" >&2
  exit 1
fi

mkdir -p "$REPO_ROOT/client"
cd "$REPO_ROOT"

CXX="$CC"
CXXFLAGS=(-std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-unused-variable
          -D_GLIBCXX_USE_C99=0
          -include client/cpp/protocol/ce_atoll_patch.h)
INC=(-I"$REPO_ROOT/client/include" -I"$REPO_ROOT/client/cpp")

# A single TU: only define _WIN32_WCE; ce_main.cpp wraps whole body in #ifdef.
SRC=(
  client/cpp/protocol/aes_sbox_data.cpp
  client/cpp/protocol/connection.cpp
  client/cpp/protocol/usb_transport.cpp
  client/cpp/ce_stubs.cpp
  client/cpp/ce_main.cpp
)

echo ">> [1/1] carlinkit_ce.exe"
"$CXX" "${CXXFLAGS[@]}" "${INC[@]}" \
  -DCARLINKIT_CE=1 \
  "${SRC[@]}" \
  -o client/carlinkit_ce.exe \
  -static-libgcc -static-libstdc++ -lws2

"$STRIP" client/carlinkit_ce.exe

ARCH=$("$CC" -dumpmachine)
SZ=$(wc -c < client/carlinkit_ce.exe)
echo "OK: client/carlinkit_ce.exe  $ARCH  $(($SZ / 1024)) KB"