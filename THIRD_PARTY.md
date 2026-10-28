# Third-party code

Every library Roda ships or links, with its version, licence and source. Licence texts are in
[`licenses/`](licenses). All are compiled from the sources in `third_party/` into Roda.exe (static);
none of them is GPL or LGPL.

| Component | Version | Licence | Source (archive SHA-256) | Used for |
|---|---|---|---|---|
| libwebp (incl. sharpyuv) | 1.5.0 | BSD-3-Clause, plus Google's patent grant (`PATENTS`) | https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz (`668c9aba…ee30f37`*) | WebP decode and encode |
| lunasvg | 3.2.1 | MIT | https://github.com/sammycage/lunasvg/archive/refs/tags/v3.2.1.tar.gz (`3420175c…5c646af8`*) | Reading SVG (drawn to pixels) |
| plutovg (lunasvg's renderer) | commit 5e4712cf (0.0.13) | MIT; includes FreeType rasteriser code under the FreeType License (FTL) and stb_truetype / stb_image (public domain or MIT) | https://github.com/sammycage/plutovg/archive/5e4712cf873b0c7829a4a6157763e2ad3ac49164.tar.gz (`556c3a1d…d349fa234`*) | Drawing SVG |
| miniz | 3.0.2 | MIT | https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip (`ada38db0…5332c5`*) | ZIP read/write, deflate for PDF, gzip |

\* Shortened; full hashes:
- libwebp: `668c9aba45565e24c27e17f7aaf7060a399f7f31dba6c97a044e1feacb930f37`
- lunasvg: `3420175c9632007edfcd0198001abc116c5c646af8e928d393cd029985cc4ee8`
- plutovg: `556c3a1da41154516340f987bffe872dde7b596a4908e29b3b29489d349fa234`
- miniz: `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`

The vendored copies are trimmed to what Roda compiles (sources, headers, licence files); nothing in them
is modified.

The FreeType License asks for credit in the documentation: *Portions of this software are copyright
© The FreeType Project (www.freetype.org). All rights reserved.*

Code reused from the author's own project [Ampaian](https://github.com/mgtx-a/ampaian)
(`gfx`, `anim`, parts of `util` and `app`) is the same author's work and not a third-party dependency.

Windows components used through their system DLLs (not shipped): Win32, OLE, Direct2D, DirectWrite,
WIC (JPEG, PNG, GIF, BMP, TIFF, ICO, and HEIC/AVIF when Microsoft's free extensions are installed),
the Shell (IFileOperation, the folder picker).
