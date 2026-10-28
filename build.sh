#!/usr/bin/env bash
# Builds Roda with the portable Zig toolchain, for x64 and ARM64. Usage: ./build.sh [out-dir] [x64|arm64|all]
# Output: <out>/x64/Roda.exe and <out>/arm64/Roda.exe
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ZIG="${ZIG:-/d/native-build/toolchain/zig-x86_64-windows-0.17.0/zig.exe}"   # CI sets ZIG=zig
OUT="${1:-$HERE/out}"
WHICH="${2:-all}"
cd "$HERE"

build() {   # build <name> <zig target>
  mkdir -p "$OUT/$1"
  "$ZIG" c++ -O2 -std=c++20 -target "$2" -fms-extensions -Wno-deprecated-declarations \
    -Wl,--subsystem,windows -s \
    src/*.cpp res/app.rc -o "$OUT/$1/Roda.exe" \
    -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lshell32 -lshlwapi -luser32 -lgdi32 -lwinmm -ladvapi32 -luuid -lpsapi -ldwmapi
  rm -f "$OUT/$1/Roda.pdb" "$OUT/$1/Roda.lib"
  printf '%-6s %s KB  %s\n' "$1" "$(( $(stat -c %s "$OUT/$1/Roda.exe") / 1024 ))" "$OUT/$1/Roda.exe"
}

[ "$WHICH" = all ] || [ "$WHICH" = x64 ] && build x64 x86_64-windows-gnu
[ "$WHICH" = all ] || [ "$WHICH" = arm64 ] && build arm64 aarch64-windows-gnu
exit 0
