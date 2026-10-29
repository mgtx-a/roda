# Third-party code

Every library Roda ships or links, with its version, licence and source. Licence texts are in
[`licenses/`](licenses). All are compiled from the sources in `third_party/` into Roda.exe (static);
none of them is GPL or LGPL.

| Component | Version | Licence | Source (archive SHA-256) | Used for |
|---|---|---|---|---|
| libwebp (incl. sharpyuv) | 1.5.0 | BSD-3-Clause, plus Google's patent grant (`PATENTS`) | https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz (`668c9aba…ee30f37`*) | WebP decode and encode |
| lunasvg | 3.2.1 | MIT | https://github.com/sammycage/lunasvg/archive/refs/tags/v3.2.1.tar.gz (`3420175c…5c646af8`*) | Reading SVG (drawn to pixels) |
| plutovg (lunasvg's renderer) | commit 5e4712cf (0.0.13) | MIT; includes FreeType rasteriser code under the FreeType License (FTL) and stb_truetype / stb_image (public domain or MIT) | https://github.com/sammycage/plutovg/archive/5e4712cf873b0c7829a4a6157763e2ad3ac49164.tar.gz (`556c3a1d…d349fa234`*) | Drawing SVG |
| miniz | 3.0.2 | MIT | https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip (`ada38db0…5332c5`*) | ZIP read/write, deflate for PDF, gzip, Office files |
| pugixml | 1.16 | MIT | https://github.com/zeux/pugixml/archive/refs/tags/v1.16.tar.gz (`357bcab8…65090219`*) | Reading DOCX, XLSX and ODS |
| md4c | 0.5.2 | MIT | https://github.com/mity/md4c/archive/refs/tags/release-0.5.2.tar.gz (`55d0111d…090ef21`*) | Reading Markdown, Markdown to HTML |
| libxls | 1.6.3 | BSD-2-Clause | https://github.com/libxls/libxls/archive/refs/tags/v1.6.3.tar.gz (`587c9f0e…6eed7`*) | Reading old .xls workbooks |

**PDFium** ships separately, as `engines\pdfium.dll` in the Full build (Roda loads it by full path,
only in a worker, only for PDFs; without it the PDF options are hidden):

| Component | Version | Licence | Source (archive SHA-256) |
|---|---|---|---|
| PDFium, prebuilt by bblanchon/pdfium-binaries | chromium/8100 | BSD-3-Clause (PDFium) and Apache-2.0; the build's own parts under their licences: FreeType (FTL), libjpeg-turbo (IJG/BSD), OpenJPEG (BSD-2), libpng, zlib, lcms (MIT), HarfBuzz (MIT), ICU (Unicode), Abseil (Apache-2.0), AGG, fast_float, dragonbox, simdutf | https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium%2F8100 — x64 `51621bcc1d9be9ea448f4b84b0516862b928e3968d3fca899af1a6d6e2848d6d`, ARM64 `cca9b28d406ae2a902b0b710a92ab699c4c2931564ab6efe44630014d492a9ca` |

Its licence texts are in [`licenses/pdfium/`](licenses/pdfium). (ICU's text mentions the GPL only for
its build scripts, which are not part of the DLL.) Its headers are vendored in `third_party/pdfium`; the DLL
is downloaded by `build.sh` and checked against the SHA-256 above.

**FFmpeg** ships separately too, as `engines\ffmpeg\` in the Full build (`ffmpeg.exe`, `ffprobe.exe` and
their DLLs; Roda runs them from a worker for audio and video only; without them those options are hidden):

| Component | Version | Licence | Source (archive SHA-256) |
|---|---|---|---|
| FFmpeg, our own trimmed shared build (`tools/ffmpeg/build.sh`) | 8.1.3 | LGPL-2.1-or-later (no `--enable-gpl`, no `--enable-nonfree`) | https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz `7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3` |
| LAME (MP3 encoder, inside avcodec) | 3.100 | LGPL-2.0-or-later | https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz `ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e` |
| Opus | 1.5.2 | BSD-3-Clause | https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` |
| libogg | 1.3.5 | BSD-3-Clause | https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz `c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705` |
| libvorbis | 1.3.7 | BSD-3-Clause | https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz `b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b` |
| libvpx (VP9 encoder) | 1.16.0 | BSD-3-Clause + patent grant | https://github.com/webmproject/libvpx/archive/refs/tags/v1.16.0.tar.gz `7a479a3c66b9f5d5542a4c6a1b7d3768a983b1e5c14c60a9396edc9b649e015c` |
| dav1d (AV1 decoder) | 1.5.3 | BSD-2-Clause | https://github.com/videolan/dav1d/archive/refs/tags/1.5.3.tar.gz `cbe212b02faf8c6eed5b6d55ef8a6e363aaab83f15112e960701a9c3df813686` |
| zlib | 1.3.1 | zlib | https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` |

Built in CI on Linux with llvm-mingw 20261006 (`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`).
It contains only the encoders, formats and filters Roda uses (decoding is broad), and no network code
at all (`--disable-network`, protocols `file` and `pipe` only). About 14 MB on x64, 9 MB on ARM64.

LGPL: the DLLs are separate files that anyone may replace with their own build of the same FFmpeg
version; `tools/ffmpeg/build.sh` rebuilds them exactly from the sources above. The licence texts ship in
`engines\ffmpeg\licenses\` and are in [`licenses/ffmpeg/`](licenses/ffmpeg).

\* Shortened; full hashes:
- libwebp: `668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37`
- lunasvg: `3420175c9632007edfcd0198001abc116c5c646af8e928d393cd029985cc4ee8`
- plutovg: `556c3a1da41154516340f987bffe872dde7b596a4908e29b3b29489d349fa234`
- miniz: `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`
- pugixml: `357bcab8877dc9943f355d3a72daba1b053238ba955f50fa81586afb65090219`
- md4c: `55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21`
- libxls: `587c9f0ebb5647eb68ec1e0ed8c3f7f6102622d6dd83473a21d3a36dee04eed7`

The vendored copies are trimmed to what Roda compiles (sources, headers, licence files); nothing in them
is modified. libxls's `locale.c` is left out and replaced by `src/shim/xls_locale.c` (Roda's own,
Windows' UTF-8 conversion), with a `config.h` of our own in `src/shim`.

The FreeType License asks for credit in the documentation: *Portions of this software are copyright
© The FreeType Project (www.freetype.org). All rights reserved.*

Code reused from the author's own project [Ampaian](https://github.com/mgtx-a/ampaian)
(`gfx`, `anim`, parts of `util` and `app`) is the same author's work and not a third-party dependency.

Windows components used through their system DLLs (not shipped): Win32, OLE, Direct2D, DirectWrite,
WIC (JPEG, PNG, GIF, BMP, TIFF, ICO, and HEIC/AVIF when Microsoft's free extensions are installed),
the Shell (IFileOperation, the folder picker).
