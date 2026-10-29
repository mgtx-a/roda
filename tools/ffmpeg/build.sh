#!/usr/bin/env bash
# Builds the trimmed FFmpeg that ships in the Full build's engines\ffmpeg\: only the codecs, formats
# and filters Roda uses, no network code (--disable-network, protocols file and pipe only), LGPL only.
# Runs on Linux (GitHub's ubuntu machines; needs curl, make, nasm, meson, ninja, cmake, pkg-config)
# and cross-compiles with llvm-mingw. Every download is pinned by SHA-256.
#
#   tools/ffmpeg/build.sh <x64|arm64> <outdir>
#
# <outdir> gets ffmpeg.exe, ffprobe.exe, their DLLs and licenses\.
set -euo pipefail

ARCH="${1:?x64 or arm64}"
OUT="$(realpath -m "${2:?output folder}")"
case "$ARCH" in
  x64)   HOST=x86_64-w64-mingw32;  CPU=x86_64;  CMAKE_CPU=AMD64; AOM_CPU=x86_64; VPX_TARGET=x86_64-win64-gcc ;;
  arm64) HOST=aarch64-w64-mingw32; CPU=aarch64; CMAKE_CPU=ARM64; AOM_CPU=arm64;  VPX_TARGET=arm64-win64-gcc ;;
  *) echo "unknown architecture: $ARCH"; exit 1 ;;
esac
WORK="$(realpath -m "${WORK:-ffwork}")"
PREFIX="$WORK/prefix-$ARCH"
JOBS="$(nproc)"
mkdir -p "$WORK/dl" "$WORK/src-$ARCH" "$PREFIX/lib" "$PREFIX/include" "$OUT"

# name  url  sha256
SOURCES="
llvm-mingw.tar.xz https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz 5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4
ffmpeg-8.1.3.tar.xz https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz 7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
zlib-1.3.1.tar.gz https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
lame-3.100.tar.gz https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
opus-1.5.2.tar.gz https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz 65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
libogg-1.3.5.tar.xz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705
libvorbis-1.3.7.tar.xz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
libvpx-1.16.0.tar.gz https://github.com/webmproject/libvpx/archive/refs/tags/v1.16.0.tar.gz 7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c
libaom-3.15.1.tar.gz https://storage.googleapis.com/aom-releases/libaom-3.15.1.tar.gz 8ca0c52746174603500f0adb6f2a215d69c9ca2aab2acb3caa06fb791d8d01bf
dav1d-1.5.3.tar.gz https://github.com/videolan/dav1d/archive/refs/tags/1.5.3.tar.gz cbe212b02faf8c6eed5b6d55ef8a6e363aaab83f15112e960701a9c3df813686
"

fetch() {   # fetch <name>: downloads once, checks the SHA-256
  local name="$1" url sha
  read -r _ url sha <<<"$(grep "^$name " <<<"$SOURCES")"
  local file="$WORK/dl/$name"
  [ -f "$file" ] || curl -sSfL --retry 3 -o "$file" "$url"
  local got; got=$(sha256sum "$file" | cut -d' ' -f1)
  [ "$got" = "$sha" ] || { echo "$name: SHA-256 $got, expected $sha"; rm -f "$file"; exit 1; }
  echo "$file"
}

unpack() {   # unpack <name> <folder inside the archive>: a fresh copy, prints its path
  local file; file=$(fetch "$1")
  rm -rf "$WORK/src-$ARCH/$2"
  tar -xf "$file" -C "$WORK/src-$ARCH"
  echo "$WORK/src-$ARCH/$2"
}

# ---------------------------------------------------------------- the compiler
TC="$WORK/llvm-mingw"
if [ ! -x "$TC/bin/$HOST-clang" ]; then
  mkdir -p "$TC" && tar -xf "$(fetch llvm-mingw.tar.xz)" -C "$TC" --strip-components=1
  # Only static runtime libraries: the DLLs then need nothing beyond Windows itself (no
  # libwinpthread-1.dll or libc++.dll to ship next to them).
  rm -f "$TC"/*-w64-mingw32/lib/libwinpthread.dll.a "$TC"/*-w64-mingw32/lib/libpthread.dll.a \
        "$TC"/*-w64-mingw32/lib/libc++.dll.a "$TC"/*-w64-mingw32/lib/libunwind.dll.a
fi
export PATH="$TC/bin:$PATH"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""
export CC="$HOST-clang" CXX="$HOST-clang++" AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm STRIP=llvm-strip
OPT="-O2"
AUTOCONF=(--host="$HOST" --prefix="$PREFIX" --enable-static --disable-shared)

newer_config() {   # old autotools scripts may not know aarch64-w64-mingw32
  for f in config.sub config.guess; do
    [ -f "/usr/share/misc/$f" ] && find . -name "$f" -exec cp "/usr/share/misc/$f" {} \;
  done
  return 0
}

# ---------------------------------------------------------------- the libraries, static
build_zlib() {
  local d; d=$(unpack zlib-1.3.1.tar.gz zlib-1.3.1)
  (cd "$d" && for c in adler32 compress crc32 deflate infback inffast inflate inftrees trees uncompr zutil; do
      $CC $OPT -c "$c.c" -o "$c.o"; done
   $AR rcs libz.a ./*.o && cp libz.a "$PREFIX/lib/" && mkdir -p "$PREFIX/include" && cp zlib.h zconf.h "$PREFIX/include/")
}

build_lame() {
  local d; d=$(unpack lame-3.100.tar.gz lame-3.100)
  (cd "$d" && newer_config && CFLAGS="$OPT" ./configure "${AUTOCONF[@]}" --disable-frontend --disable-decoder --disable-gtktest \
   && make -j"$JOBS" -C libmp3lame && make -C libmp3lame install && make -C include install)
}

build_ogg() {
  local d; d=$(unpack libogg-1.3.5.tar.xz libogg-1.3.5)
  (cd "$d" && newer_config && CFLAGS="$OPT" ./configure "${AUTOCONF[@]}" && make -j"$JOBS" -C src && make -C src install \
   && make -C include install && make install-pkgconfigDATA)
}

build_vorbis() {
  local d; d=$(unpack libvorbis-1.3.7.tar.xz libvorbis-1.3.7)
  # Its configure adds a GCC-only flag when it sees an x86 "gcc"; clang refuses it.
  (cd "$d" && newer_config && sed -i 's/-mno-ieee-fp//g' configure \
   && CFLAGS="$OPT" ./configure "${AUTOCONF[@]}" --with-ogg="$PREFIX" --disable-docs --disable-examples --disable-oggtest \
   && make -j"$JOBS" -C lib && make -C lib install && make -C include install && make install-pkgconfigDATA)
}

build_opus() {
  local d; d=$(unpack opus-1.5.2.tar.gz opus-1.5.2)
  # Windows on ARM can't ask the CPU what it has at run time; every such CPU has NEON anyway.
  local rtcd=(); [ "$ARCH" = arm64 ] && rtcd=(--disable-rtcd)
  (cd "$d" && newer_config && CFLAGS="$OPT" ./configure "${AUTOCONF[@]}" "${rtcd[@]}" --disable-doc --disable-extra-programs \
   && make -j"$JOBS" && make install)
}

build_vpx() {   # the VP9 encoder only: FFmpeg decodes VP8 and VP9 itself
  local d; d=$(unpack libvpx-1.16.0.tar.gz libvpx-1.16.0)
  local as=(); [ "$ARCH" = x64 ] && as=(--as=nasm)
  (cd "$d" && LD="$CC" ./configure --target="$VPX_TARGET" --prefix="$PREFIX" "${as[@]}" \
      --disable-examples --disable-tools --disable-docs --disable-unit-tests \
      --disable-vp8 --disable-vp9-decoder --enable-vp9-encoder --disable-shared --enable-static \
   && make -j"$JOBS" && make install)
}

build_aom() {   # the AV1 encoder only (AVIF pictures); dav1d decodes AV1
  local d; d=$(unpack libaom-3.15.1.tar.gz libaom-3.15.1)
  local nasm=-DENABLE_NASM=OFF; [ "$ARCH" = x64 ] && nasm=-DENABLE_NASM=ON
  cmake -S "$d" -B "$d/out" -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR="$CMAKE_CPU" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_RC_COMPILER="$HOST-windres" -DCMAKE_AR="$TC/bin/llvm-ar" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DAOM_TARGET_CPU="$AOM_CPU" "$nasm" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_DOCS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF -DENABLE_TOOLS=OFF \
    -DCONFIG_AV1_DECODER=0 -DCONFIG_AV1_HIGHBITDEPTH=0 >/dev/null
  cmake --build "$d/out" -j"$JOBS" && cmake --install "$d/out" >/dev/null
}

build_dav1d() {
  local d; d=$(unpack dav1d-1.5.3.tar.gz dav1d-1.5.3)
  cat > "$d/cross.txt" <<EOF
[binaries]
c = '$HOST-clang'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = '$HOST-windres'
nasm = 'nasm'
[host_machine]
system = 'windows'
cpu_family = '$CPU'
cpu = '$CPU'
endian = 'little'
EOF
  meson setup "$d/out" "$d" --cross-file "$d/cross.txt" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false >/dev/null
  ninja -C "$d/out" && ninja -C "$d/out" install >/dev/null
}

# ---------------------------------------------------------------- FFmpeg itself
# What Roda asks of it (src/conv/media.cpp, src/conv/image.cpp). Decoding is broad: anything a person
# might drop. Encoding is exactly the encoders Roda names. d3d11va only because the Media Foundation
# encoder (h264_mf) is written against it; Windows has Direct3D 11 itself.
ENCODERS="libmp3lame aac flac libopus libvorbis wmav2 pcm_s16le pcm_s16be h264_mf libvpx_vp9 libaom_av1 mpeg4 wmv2 gif png"
DECODERS="h264 hevc mpeg4 msmpeg4v1 msmpeg4v2 msmpeg4v3 mpeg1video mpeg2video mjpeg mjpegb vp8 vp9 libdav1d theora
  wmv1 wmv2 wmv3 vc1 h263 flv prores dnxhd dvvideo png gif webp bmp rawvideo svq3 cinepak
  aac aac_latm mp3 mp3float mp2 mp1 ac3 eac3 dca flac alac opus vorbis wmav1 wmav2 wmapro wmalossless wmavoice
  amrnb amrwb truehd mlp ape wavpack tta adpcm_ima_wav adpcm_ms gsm_ms
  pcm_s16le pcm_s16be pcm_s24le pcm_s24be pcm_s32le pcm_s32be pcm_f32le pcm_f32be pcm_f64le pcm_u8 pcm_s8 pcm_alaw pcm_mulaw pcm_bluray pcm_dvd"
DEMUXERS="image2 image_png_pipe mov matroska avi asf flv mpegps mpegts mpegvideo h264 hevc m4v mp3 aac ac3 eac3 dts flac ogg wav w64 aiff caf amr ape wv tta truehd gif mjpeg dv mxf ivf obu"
MUXERS="mp4 mov matroska webm avi asf gif mp3 ipod wav flac ogg opus aiff image2pipe avif"
PARSERS="h264 hevc mpeg4video mpegvideo mpegaudio aac aac_latm ac3 dca flac opus vorbis vp8 vp9 av1 mjpeg png gif vc1 h263 mlp"
FILTERS="scale crop fps split palettegen paletteuse aresample aformat concat format null anull trim atrim setpts asetpts hflip vflip transpose"

list() { echo "$2" | tr -s ' \n' '\n' | grep -v '^$' | sed "s/^/--enable-$1=/" ; }

build_ffmpeg() {
  local d; d=$(unpack ffmpeg-8.1.3.tar.xz ffmpeg-8.1.3)
  (cd "$d" && ./configure --prefix="$PREFIX/ffmpeg" --enable-cross-compile --target-os=mingw32 --arch="$CPU" \
      --cross-prefix="$HOST-" --cc="$CC" --cxx="$CXX" --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
      --pkg-config=pkg-config --pkg-config-flags=--static \
      --extra-cflags="-I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
      --enable-shared --disable-static --disable-debug --disable-doc --disable-ffplay --disable-avdevice \
      --disable-everything --disable-autodetect --disable-network \
      --enable-w32threads --enable-mediafoundation --enable-d3d11va --enable-zlib \
      --enable-libmp3lame --enable-libopus --enable-libvorbis --enable-libvpx --enable-libaom --enable-libdav1d \
      --enable-protocol=file --enable-protocol=pipe --enable-bsfs \
      $(list encoder "$ENCODERS") $(list decoder "$DECODERS") $(list demuxer "$DEMUXERS") \
      $(list muxer "$MUXERS") $(list parser "$PARSERS") $(list filter "$FILTERS") > "$WORK/configure-$ARCH.txt")
  # configure only warns when a name matches nothing: fail loudly instead.
  if grep -i "did not match anything" "$WORK/configure-$ARCH.txt"; then exit 1; fi
  for lib in libmp3lame libopus libvorbis libvpx libaom libdav1d mediafoundation zlib; do
    sed -n '/^External libraries/,/^Enabled programs/p' "$WORK/configure-$ARCH.txt" | grep -qw "$lib" || { echo "configure did not take $lib"; exit 1; }
  done
  make -C "$d" -j"$JOBS" && make -C "$d" install
}

collect() {
  rm -rf "$OUT" && mkdir -p "$OUT/licenses"
  cp "$PREFIX/ffmpeg/bin/ffmpeg.exe" "$PREFIX/ffmpeg/bin/ffprobe.exe" "$PREFIX/ffmpeg/bin/"*.dll "$OUT/"
  llvm-strip "$OUT"/*.exe "$OUT"/*.dll
  local s="$WORK/src-$ARCH"
  cp "$s/ffmpeg-8.1.3/COPYING.LGPLv2.1" "$OUT/licenses/FFmpeg-LGPL-2.1.txt"
  cp "$s/zlib-1.3.1/LICENSE"          "$OUT/licenses/zlib.txt"
  cp "$s/lame-3.100/COPYING"          "$OUT/licenses/LAME-LGPL-2.txt"
  cp "$s/opus-1.5.2/COPYING"          "$OUT/licenses/Opus.txt"
  cp "$s/libogg-1.3.5/COPYING"        "$OUT/licenses/Ogg.txt"
  cp "$s/libvorbis-1.3.7/COPYING"     "$OUT/licenses/Vorbis.txt"
  cp "$s/libvpx-1.16.0/LICENSE"       "$OUT/licenses/libvpx.txt"
  cp "$s/libvpx-1.16.0/PATENTS"       "$OUT/licenses/libvpx-PATENTS.txt"
  cp "$s/libaom-3.15.1/LICENSE"       "$OUT/licenses/libaom.txt"
  cp "$s/libaom-3.15.1/PATENTS"       "$OUT/licenses/libaom-PATENTS.txt"
  cp "$s/dav1d-1.5.3/COPYING"         "$OUT/licenses/dav1d.txt"
  # Nothing but Windows' own DLLs (and our own) may be needed.
  for f in "$OUT"/*.exe "$OUT"/*.dll; do
    llvm-objdump -p "$f" | sed -n 's/.*DLL Name: //p' \
      | grep -viE '^(av|sw)[a-z]*-[0-9]+\.dll$|^(ucrtbase|msvcrt|kernel32|user32|advapi32|ole32|oleaut32|shell32|bcrypt|mfplat|mf|mfuuid|propsys|shlwapi|ws2_32|secur32|gdi32|psapi|strmiids)\.dll$|^api-ms-win-' \
      && { echo "$f needs a DLL Windows doesn't have"; exit 1; } || true
  done
  du -b "$OUT"/*.exe "$OUT"/*.dll | sort -n
  echo "total: $(du -cb "$OUT"/*.exe "$OUT"/*.dll | tail -1 | cut -f1) bytes"
}

build_zlib
build_lame
build_ogg
build_vorbis
build_opus
build_vpx
build_aom
build_dav1d
build_ffmpeg
collect
