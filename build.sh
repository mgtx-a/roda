#!/usr/bin/env bash
# Builds Roda with the portable Zig toolchain, for x64 and ARM64. Usage: ./build.sh [out-dir] [x64|arm64|all]
# Output: <out>/x64/Roda.exe (+ engines/pdfium.dll) and <out>/arm64/Roda.exe (+ engines/pdfium.dll)
# Third-party code (third_party/) is compiled once per architecture into <out>/obj/<arch>/tp and reused.
# PDFium is a prebuilt DLL (bblanchon/pdfium-binaries), downloaded once, checked against its SHA-256.
# NO_PDFIUM=1 skips it (a Lite build: Roda then hides the PDF options).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ZIG="${ZIG:-/d/native-build/toolchain/zig-x86_64-windows-0.17.0/zig.exe}"   # CI sets ZIG=zig
OUT="${1:-$HERE/out}"
WHICH="${2:-all}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
cd "$HERE"

TP=third_party
WEBP=$TP/libwebp-1.5.0
LUNA=$TP/lunasvg-3.2.1
MINIZ=$TP/miniz-3.0.2
PUGI=$TP/pugixml-1.16
MD4C=$TP/md4c-release-0.5.2
XLS=$TP/libxls-1.6.3
PDFIUM=$TP/pdfium
INCLUDES="-I$WEBP -I$WEBP/src -I$LUNA/include -I$LUNA/plutovg/include -I$MINIZ -I$PUGI/src -I$MD4C/src -I$XLS/include -I$PDFIUM/include -Isrc/shim"
DEFS="-DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC -DPUGIXML_NO_EXCEPTIONS -DPUGIXML_NO_XPATH"

PDFIUM_TAG="chromium/8100"
PDFIUM_SHA_x64=51621bcc1d9be9ea448f4b84b0516862b928e3968d3fca899af1a6d6e2848d6d
PDFIUM_SHA_arm64=cca9b28d406ae2a902b0b710a92ab699c4c2931564ab6efe44630014d492a9ca

tp_sources() {
  ls $WEBP/src/dec/*.c $WEBP/src/enc/*.c $WEBP/src/dsp/*.c $WEBP/src/utils/*.c $WEBP/sharpyuv/*.c \
     $LUNA/source/*.cpp $LUNA/plutovg/source/*.c $MINIZ/miniz.c $PUGI/src/pugixml.cpp \
     $MD4C/src/md4c.c $MD4C/src/md4c-html.c $MD4C/src/entity.c $XLS/src/*.c src/shim/*.c
}

# compile <zig target> <obj dir> <source>: one object, skipped when it is newer than its source.
compile() {
  local target="$1" dir="$2" src="$3"
  local obj="$dir/$(echo "$src" | tr '/.' '__').o"
  [ "$obj" -nt "$src" ] && [ "$obj" -nt build.sh ] && { echo "$obj"; return; }
  # Small code everywhere (the exe must stay under 2 MB), except WebP, where encoding speed shows.
  local opt=-Os
  case "$src" in *libwebp*) opt=-O2 ;; esac
  case "$src" in
    *.c)   "$ZIG" cc  $opt -ffunction-sections -fdata-sections -std=c11   -target "$target" $INCLUDES $DEFS -w -c "$src" -o "$obj" ;;
    *.cpp) "$ZIG" c++ $opt -ffunction-sections -fdata-sections -std=c++20 -target "$target" $INCLUDES $DEFS -fms-extensions -Wno-deprecated-declarations -c "$src" -o "$obj" ;;
  esac
  echo "$obj"
}
export -f compile
export ZIG INCLUDES DEFS

# pdfium <arch> <dest dir>: the DLL into dest/engines, from the cache in third_party/pdfium-bin (not in git).
pdfium() {
  [ -n "${NO_PDFIUM:-}" ] && return
  local arch="$1" dest="$2" cache="$TP/pdfium-bin/$1"
  local want; eval want=\$PDFIUM_SHA_$arch
  if [ ! -f "$cache/pdfium.dll" ]; then
    mkdir -p "$cache"
    local tgz="$cache/pdfium.tgz"
    curl -sSfL -o "$tgz" "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_TAG/\//%2F}/pdfium-win-$arch.tgz"
    local got; got=$(sha256sum "$tgz" | cut -d' ' -f1)
    [ "$got" = "$want" ] || { echo "pdfium-win-$arch.tgz: SHA-256 $got, expected $want"; rm -f "$tgz"; exit 1; }
    tar xzf "$tgz" -C "$cache" bin/pdfium.dll && mv "$cache/bin/pdfium.dll" "$cache/" && rmdir "$cache/bin" && rm -f "$tgz"
  fi
  mkdir -p "$dest/engines"
  cp "$cache/pdfium.dll" "$dest/engines/"
}

build() {   # build <name> <zig target>
  local name="$1" target="$2"
  local tpdir="$OUT/obj/$name/tp" srcdir="$OUT/obj/$name/src"
  mkdir -p "$OUT/$name" "$tpdir" "$srcdir"
  # Our headers change often; rebuild our own objects when any header is newer than the object.
  local newest_h; newest_h=$(ls -t src/*.h src/conv/*.h 2>/dev/null | head -1)
  for o in "$srcdir"/*.o; do [ -e "$o" ] && [ "$newest_h" -nt "$o" ] && rm -f "$o"; done
  tp_sources | xargs -P "$JOBS" -I{} bash -c 'compile "$0" "$1" "$2"' "$target" "$tpdir" {} > "$OUT/obj/$name/tp.list"
  ls src/*.cpp src/conv/*.cpp | xargs -P "$JOBS" -I{} bash -c 'compile "$0" "$1" "$2"' "$target" "$srcdir" {} > "$OUT/obj/$name/src.list"
  "$ZIG" c++ -O2 -target "$target" -Wl,--subsystem,windows -Wl,--gc-sections -s \
    $(cat "$OUT/obj/$name/src.list" "$OUT/obj/$name/tp.list") res/app.rc -o "$OUT/$name/Roda.exe" \
    -ld2d1 -ldwrite -lwindowscodecs -lole32 -loleaut32 -lshell32 -lshlwapi -luser32 -lgdi32 -lwinmm -ladvapi32 -luuid -lpsapi -ldwmapi
  rm -f "$OUT/$name/Roda.pdb" "$OUT/$name/Roda.lib"
  pdfium "$name" "$OUT/$name"
  printf '%-6s %s KB  %s\n' "$name" "$(( $(stat -c %s "$OUT/$name/Roda.exe") / 1024 ))" "$OUT/$name/Roda.exe"
}

[ "$WHICH" = all ] || [ "$WHICH" = x64 ] && build x64 x86_64-windows-gnu
[ "$WHICH" = all ] || [ "$WHICH" = arm64 ] && build arm64 aarch64-windows-gnu
exit 0
