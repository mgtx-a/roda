#!/usr/bin/env bash
# Runs Roda's converters through the CLI over tests/corpus and checks the results: exit code, output
# present, output decodes again with the expected size, broken files fail cleanly, nothing is left behind.
#   tests/run_tests.sh [path/to/Roda.exe]        (default: out/x64/Roda.exe)
# Works in Git Bash on Windows (CI's windows-latest included).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
EXE="${1:-$ROOT/out/x64/Roda.exe}"
CORPUS="$HERE/corpus"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0
win() { cygpath -w "$1"; }

ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }

# roda <args...>: runs with a 60 s limit; output in $OUT, exit code in $RC.
roda() { OUT="$(timeout 60 "$EXE" "$@" 2>&1)"; RC=$?; }
info() { roda --info "$(win "$1")"; INFO="$OUT"; }
field() { echo "$INFO" | sed -n "s/.*\"$1\":\([^,}]*\).*/\1/p" | tr -d '"'; }

convert() {   # convert <in> <out> [cli args...]: succeeds and the output reads back
  local in="$1" out="$2"; shift 2
  roda --convert "$(win "$in")" "$(win "$out")" "$@"
  if [ $RC -ne 0 ]; then bad "$(basename "$in") -> $(basename "$out") $*: rc=$RC $OUT"; return 1; fi
  if [ ! -s "$out" ]; then bad "$(basename "$out"): missing or empty"; return 1; fi
  info "$out"
  if [ $RC -ne 0 ]; then bad "$(basename "$out") does not read back: $INFO"; return 1; fi
  return 0
}

dims() { info "$1"; echo "$(field w)x$(field h)"; }

echo "Roda: $EXE"
[ -x "$EXE" ] || { echo "no exe"; exit 2; }

# ---------------------------------------------------------------- every image into every format
INPUTS="photo.jpg photo.png transparent.png photo.bmp photo.tiff icon.ico photo.webp lossless.webp animated.gif cmyk.jpg gray16.png rotated-exif.jpg drawing.svg sized.svg 测试 файл.png really-a-png.jpg"
mkdir -p "$WORK/conv"
IFS=$'\n'
for in in photo.jpg photo.png transparent.png photo.bmp photo.tiff icon.ico photo.webp lossless.webp animated.gif cmyk.jpg gray16.png rotated-exif.jpg drawing.svg sized.svg "测试 файл.png" really-a-png.jpg; do
  src="$CORPUS/$in"
  want="$(dims "$src")"
  [ "$want" = "x" ] && { bad "$in does not decode: $INFO"; continue; }
  for ext in jpg png webp tiff bmp ico pdf; do
    out="$WORK/conv/${in%.*}.$in.$ext"
    convert "$src" "$out" || continue
    case "$ext" in
      pdf) [ "$(field pages)" = "1" ] && [ "$(field wellFormed)" = "true" ] && ok || bad "$out: $INFO" ;;
      ico) [ "$(field kind)" = "ICO" ] && ok || bad "$out: $INFO" ;;
      *)   got="$(field w)x$(field h)"; [ "$got" = "$want" ] && ok || bad "$in -> $ext: size $got, expected $want" ;;
    esac
  done
done
unset IFS

# Specific promises.
[ "$(dims "$CORPUS/rotated-exif.jpg")" = "100x200" ] && ok || bad "EXIF orientation not applied: $(dims "$CORPUS/rotated-exif.jpg")"
info "$CORPUS/really-a-png.jpg"; [ "$(field kind)" = "PNG" ] && ok || bad "wrong extension: kind $(field kind)"
info "$WORK/conv/transparent.transparent.png.webp"; [ "$(field alpha)" = "true" ] && ok || bad "WebP lost transparency"
info "$WORK/conv/transparent.transparent.png.jpg"; [ "$(field alpha)" = "false" ] && ok || bad "JPG has alpha?"

# ---------------------------------------------------------------- tools
mkdir -p "$WORK/tools"
for in in photo.jpg transparent.png photo.webp; do
  src="$CORPUS/$in"; base="${in%.*}"; ext="${in##*.}"
  read -r W H <<<"$(dims "$src" | tr x ' ')"
  t="$WORK/tools/$base"
  convert "$src" "$t-c.$ext" --op tool.compress --option q=50 && ok
  convert "$src" "$t-50.$ext" --op tool.resize50 && { [ "$(field w)x$(field h)" = "$((W / 2))x$((H / 2))" ] && ok || bad "$in 50%: $(field w)x$(field h)"; }
  convert "$src" "$t-1920.$ext" --op tool.resize1920 && { [ "$(field w)" = "$W" ] && ok || bad "$in was enlarged"; }
  convert "$src" "$t-w100.$ext" --op tool.resize --option w=100 && { [ "$(field w)" = "100" ] && ok || bad "$in w=100: $(field w)"; }
  convert "$src" "$t-crop.$ext" --op tool.crop --option crop=10,10,50,40 && { [ "$(field w)x$(field h)" = "50x40" ] && ok || bad "$in crop: $(field w)x$(field h)"; }
  convert "$src" "$t-rot.$ext" --op tool.rotate && { [ "$(field w)x$(field h)" = "${H}x${W}" ] && ok || bad "$in rotate: $(field w)x$(field h)"; }
  convert "$src" "$t-flip.$ext" --op tool.flip && ok
  convert "$src" "$t-strip.$ext" --op tool.strip && ok
done
# Strip info is lossless for JPEG: the image data stays byte-identical, only headers go.
roda --convert "$(win "$CORPUS/rotated-exif.jpg")" "$(win "$WORK/tools/noexif.jpg")" --op tool.strip
[ $RC -eq 0 ] && [ "$(stat -c %s "$WORK/tools/noexif.jpg")" -lt "$(stat -c %s "$CORPUS/rotated-exif.jpg")" ] && ok || bad "strip did not shrink the JPEG: $OUT"
# Compress keeps EXIF (by default) but resets the orientation, as the pixels are already upright.
convert "$CORPUS/rotated-exif.jpg" "$WORK/tools/rot-c.jpg" --op tool.compress --option q=60 && { [ "$(field w)x$(field h)" = "100x200" ] && ok || bad "compress turned the photo: $(field w)x$(field h)"; }
# Merge into one PDF.
roda --convert "$(win "$CORPUS/photo.jpg")" "$(win "$CORPUS/transparent.png")" "$(win "$CORPUS/photo.webp")" "$(win "$WORK/merged.pdf")"
info "$WORK/merged.pdf"; [ $RC -eq 0 ] && [ "$(field pages)" = "3" ] && ok || bad "merge PDF: $INFO"

# ---------------------------------------------------------------- archives
mkdir -p "$WORK/arch"
for a in zip tar tar.gz; do
  roda --convert "$(win "$CORPUS/folder")" "$(win "$CORPUS/photo.png")" "$(win "$WORK/arch/pack.$a")"
  [ $RC -eq 0 ] || { bad "pack $a: $OUT"; continue; }
  mkdir -p "$WORK/arch/x-$a"
  roda --convert "$(win "$WORK/arch/pack.$a")" "$(win "$WORK/arch/x-$a")\\"
  [ $RC -eq 0 ] || { bad "extract $a: $OUT"; continue; }
  diff -r "$CORPUS/folder" "$WORK/arch/x-$a/pack/folder" >/dev/null && cmp -s "$CORPUS/photo.png" "$WORK/arch/x-$a/pack/photo.png" && ok || bad "$a round trip differs"
done
mkdir -p "$WORK/arch/z"
roda --convert "$(win "$CORPUS/sample.zip")" "$(win "$WORK/arch/z")\\"
[ $RC -eq 0 ] && [ -f "$WORK/arch/z/sample/folder/sub dir/über.txt" ] && ok || bad "extract zip with a non-ASCII name: $OUT"
mkdir -p "$WORK/arch/evil"
roda --convert "$(win "$CORPUS/evil.zip")" "$(win "$WORK/arch/evil")\\"
[ -f "$WORK/arch/evil/evil/ok.txt" ] && [ ! -e "$WORK/arch/escaped.txt" ] && [ ! -e "$WORK/arch/evil/escaped.txt" ] && ok || bad "zip with ../ escaped its folder"

# ---------------------------------------------------------------- PDFs (when engines/pdfium.dll is next to the exe)
has() { grep -qF -- "$2" "$1" && ok || bad "$(basename "$1") lacks '$2'"; }
lacks() { grep -qF -- "$2" "$1" && bad "$(basename "$1") has '$2'" || ok; }
count() { ls "$1" 2>/dev/null | wc -l | tr -d ' '; }
mkdir -p "$WORK/pdf"
if [ -f "$(dirname "$EXE")/engines/pdfium.dll" ]; then
  P="$WORK/pdf"
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$P")\\" --op to.png
  [ $RC -eq 0 ] && [ "$(count "$P/report")" = "3" ] && ok || bad "PDF to PNG: $OUT"
  info "$P/report/report page 1.png"; [ "$(field w)x$(field h)" = "2480x3508" ] && ok || bad "A4 at 300 DPI is $(field w)x$(field h)"
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$P/r.jpg")" --op to.jpg; [ $RC -eq 0 ] && ok || bad "PDF to JPG: $OUT"
  convert "$CORPUS/report.pdf" "$P/r.txt" && { has "$P/r.txt" "first paragraph of the document. It is long enough"; has "$P/r.txt" "Page three."; }
  convert "$CORPUS/report.pdf" "$P/r.md" && { has "$P/r.md" "# Introduction"; has "$P/r.md" "- First point"; has "$P/r.md" "**bold**"; has "$P/r.md" "*italic*"; }
  convert "$CORPUS/report.pdf" "$P/r.docx" && { [ "$(field entries)" -ge 8 ] && ok || bad "PDF to DOCX: $INFO"; }
  convert "$P/r.docx" "$P/r-docx.txt" --op to.txt && has "$P/r-docx.txt" "Page two text."
  convert "$CORPUS/report.pdf" "$P/r.epub" && ok
  convert "$CORPUS/report.pdf" "$P/r.pptx" && { unzip -l "$P/r.pptx" | grep -q "ppt/slides/slide3.xml" && ok || bad "PPTX has no third slide"; }
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$P")\\" --op pdf.split
  [ $RC -eq 0 ] && [ "$(count "$P/report pages")" = "3" ] && ok || bad "split: $OUT"
  info "$P/report pages/report page 2.pdf"; [ "$(field pages)" = "1" ] && ok || bad "split page: $INFO"
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$P")\\" --op pdf.images
  [ $RC -eq 0 ] && [ -f "$P/report images/report image 1.jpg" ] && ok || bad "extract images: $OUT $(ls "$P/report images" 2>&1)"
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$P/rot.pdf")" --op pdf.rotate
  info "$P/rot.pdf"; [ "$(field pages)" = "3" ] && ok || bad "rotate: $INFO"
  roda --convert "$(win "$CORPUS/report.pdf")" "$(win "$CORPUS/photo.jpg")" "$(win "$CORPUS/long.pdf")" "$(win "$P/merged.pdf")"
  info "$P/merged.pdf"; [ "$(field pages)" = "304" ] && ok || bad "merge PDFs and a picture: $INFO"
  convert "$CORPUS/long.pdf" "$P/long.txt" && { [ "$(grep -c 'of 300' "$P/long.txt")" = "300" ] && ok || bad "300-page PDF text"; }
  roda --convert "$(win "$CORPUS/long.pdf")" "$(win "$P")\\" --op to.png --option dpi=36
  [ $RC -eq 0 ] && [ "$(count "$P/long")" = "300" ] && ok || bad "300 pages to PNG: $OUT"
  for bad in password.pdf broken.pdf; do
    roda --convert "$(win "$CORPUS/$bad")" "$(win "$P/$bad.txt")"
    [ $RC -eq 1 ] && [[ "$OUT" == error:* ]] && [ ! -e "$P/$bad.txt" ] && ok || bad "$bad: rc=$RC '$OUT'"
  done
  [[ "$(roda --convert "$(win "$CORPUS/password.pdf")" "$(win "$P/x.txt")"; echo "$OUT")" == *"password protected"* ]] && ok || bad "password message"
else
  echo "(no engines/pdfium.dll next to the exe: PDF tests skipped)"
fi

# ---------------------------------------------------------------- Office documents to PDF (only where Office is installed)
O="$WORK/office"; mkdir -p "$O"
for pair in "Word.Application sample.docx" "Word.Application sample.rtf" "Excel.Application sample.xlsx" "Excel.Application sample.xls"; do
  set -- $pair
  if MSYS_NO_PATHCONV=1 reg query "HKCR\\$1" >/dev/null 2>&1; then   # only reads the registry
    roda --convert "$(win "$CORPUS/$2")" "$(win "$O/$2.pdf")" --op to.pdf
    info "$O/$2.pdf"; [ $RC -eq 0 ] && [ "$(field pages)" -ge 1 ] 2>/dev/null && ok || bad "$2 to PDF through Office: $OUT $INFO"
  else
    echo "($1 not installed: $2 to PDF skipped)"
  fi
done

# ---------------------------------------------------------------- documents
D="$WORK/doc"; mkdir -p "$D"
for pair in "sample.docx html" "sample.docx md" "sample.docx txt" "sample.docx epub" "sample.md html" "sample.md docx" "sample.md txt" "sample.md epub" \
            "sample.html md" "sample.html txt" "sample.html docx" "sample.rtf txt" "sample.rtf html" "sample.rtf md" "sample.rtf docx" \
            "sample.txt docx" "sample.txt epub" "sample.txt html"; do
  set -- $pair
  roda --convert "$(win "$CORPUS/$1")" "$(win "$D/${1%.*}-${1##*.}.$2")"
  [ $RC -eq 0 ] && [ -s "$D/${1%.*}-${1##*.}.$2" ] && ok || bad "$1 -> $2: $OUT"
done
has "$D/sample-docx.md" "Plain, **bold** and *italic* words."
has "$D/sample-docx.md" "- Bullet one"
has "$D/sample-docx.md" "2. Step two"
has "$D/sample-docx.md" "| Width | 42 |"
has "$D/sample-docx.html" "data:image/png;base64,"
has "$D/sample-md.html" "<table>"
has "$D/sample-md.html" "ünïcödé"
has "$D/sample-html.md" "Café €5"
has "$D/sample-html.md" "[link](https://example.com)"
lacks "$D/sample-html.md" "no</p>"
lacks "$D/sample-html.md" "comment"
has "$D/sample-html.md" "   - inner"
has "$D/sample-rtf.txt" "café and € euro."
has "$D/sample-rtf.md" "# RTF sample"
has "$D/sample-rtf.md" "Plain, **bold** and *italic*"
# Round trips through Roda's own DOCX and EPUB.
convert "$D/sample-md.docx" "$D/md-docx.txt" --op to.txt && { has "$D/md-docx.txt" "code block"; has "$D/md-docx.txt" "nested"; }
convert "$D/sample-md.docx" "$D/md-docx.md" --op to.md && { has "$D/md-docx.md" "## List"; has "$D/md-docx.md" "1. first"; }
unzip -p "$D/sample-docx.epub" mimetype | grep -qx "application/epub+zip" && ok || bad "EPUB mimetype"
unzip -l "$D/sample-docx.epub" | grep -q "OEBPS/nav.xhtml" && ok || bad "EPUB without a table of contents"

# ---------------------------------------------------------------- spreadsheets
S="$WORK/sheet"; mkdir -p "$S"
for f in sample.xlsx sample.ods sample.csv sample.tsv sample.xls semicolon.csv; do
  for t in xlsx ods csv tsv; do
    [ "${f##*.}" = "$t" ] && continue
    out="$S/${f%.*}-${f##*.}.$t"
    roda --convert "$(win "$CORPUS/$f")" "$(win "$out")" --op to.$t
    [ $RC -eq 0 ] && ok || bad "$f -> $t: $OUT"
  done
done
# A two-sheet workbook to CSV: a folder with a file per sheet.
roda --convert "$(win "$CORPUS/sample.xlsx")" "$(win "$S")\\" --op to.csv
[ $RC -eq 0 ] && [ "$(count "$S/sample")" = "2" ] && ok || bad "two sheets to CSV: $OUT"
has "$S/sample/sample - Data.csv" '"has ""quotes"", commas"'
has "$S/sample/sample - Data.csv" "2024-05-17 14:30:00"
has "$S/sample-csv.tsv" "007"
has "$S/semicolon-csv.tsv" "bread	2,50"
has "$S/sample-ods.csv" "x,12.5"
[ "$(wc -l < "$S/sample-ods.csv")" -le 3 ] && ok || bad "ODS repeated empty rows were written out"
roda --convert "$(win "$CORPUS/sample.xls")" "$(win "$S")\\" --op to.csv
has "$S/sample (1)/sample - Data.csv" "2024-05-17"
convert "$S/sample-csv.xlsx" "$S/csv-xlsx.csv" --op to.csv && has "$S/csv-xlsx.csv" "two"

# ---------------------------------------------------------------- subtitles
T2="$WORK/sub"; mkdir -p "$T2"
convert "$CORPUS/sample.srt" "$T2/a.vtt" --op to.vtt && { has "$T2/a.vtt" "WEBVTT"; has "$T2/a.vtt" "00:00:01.000 --> 00:00:03.500"; }
convert "$T2/a.vtt" "$T2/b.srt" --op to.srt && has "$T2/b.srt" "00:00:04,000 --> 00:00:06,000"
convert "$CORPUS/sample.vtt" "$T2/c.srt" --op to.srt && { has "$T2/c.srt" "Hi & welcome."; lacks "$T2/c.srt" "align:"; }
convert "$CORPUS/sample.vtt" "$T2/d.txt" --op to.txt && has "$T2/d.txt" "Second cue."
convert "$CORPUS/sample.srt" "$T2/e.txt" --op to.txt && { has "$T2/e.txt" "Hello there."; has "$T2/e.txt" "Two lines of text."; }

# ---------------------------------------------------------------- audio and video (when engines/ffmpeg is next to the exe)
FFDIR="$(dirname "$EXE")/engines/ffmpeg"
if [ -f "$FFDIR/ffmpeg.exe" ]; then
  M="$WORK/media"; mkdir -p "$M"
  dur() { "$FFDIR/ffprobe.exe" -v error -show_entries format=duration -of csv=p=0 "$1" 2>/dev/null | tr -d '\r'; }
  near() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(a != "" && d < 0.35) }'; }
  # Every audio file into every audio format; every video into every format; a GIF into video.
  for f in tone.wav tone.mp3 tone.m4a tone.flac tone.ogg tone.opus tone.aiff tone.wma clip.mp4 clip.mov clip.mkv clip.webm clip.avi clip.wmv animated.gif; do
    for t in mp3 m4a wav flac ogg opus aiff wma mp4 mov mkv webm avi wmv gif; do
      case "$f" in
        tone.*) case $t in mp4|mov|mkv|webm|avi|wmv|gif) continue ;; esac ;;
        animated.gif) case $t in mp4|webm) ;; *) continue ;; esac ;;
      esac
      [ "${f##*.}" = "$t" ] && continue
      out="$M/${f%.*}-${f##*.}.$t"
      roda --convert "$(win "$CORPUS/$f")" "$(win "$out")" --op to.$t
      d="$(dur "$out")"
      want=2; [ "$f" = animated.gif ] && want="$(dur "$CORPUS/$f")"
      [ $RC -eq 0 ] && near "$d" "$want" && ok || bad "$f -> $t: rc=$RC duration '$d' (expected $want) $OUT"
    done
  done
  # AVIF out (and back in) through FFmpeg.
  convert "$CORPUS/photo.jpg" "$M/p.avif" --op to.avif && { [ "$(field kind)x$(field w)x$(field h)" = "AVIFx640x480" ] && ok || bad "AVIF: $INFO"; }
  convert "$M/p.avif" "$M/p-avif.png" --op to.png && { [ "$(field w)x$(field h)" = "640x480" ] && ok || bad "AVIF back to PNG: $INFO"; }
  # Copied, not re-encoded, when only the container changes (MKV of VP9+Opus into WebM: identical streams).
  roda --convert "$(win "$CORPUS/clip.mkv")" "$(win "$M/copy.webm")" --op to.webm
  [ "$(stat -c %s "$M/copy.webm")" -gt 40000 ] && [ "$(stat -c %s "$M/copy.webm")" -lt 70000 ] && ok || bad "MKV -> WebM was not a stream copy"
  # Tools.
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/t.mp4")" --op media.trim --option start=0.5 --option end=1.5
  near "$(dur "$M/t.mp4")" 1 && ok || bad "trim: $(dur "$M/t.mp4") $OUT"
  roda --convert "$(win "$CORPUS/tone.mp3")" "$(win "$CORPUS/tone.wav")" "$(win "$M/j.mp3")" --op media.merge
  near "$(dur "$M/j.mp3")" 4 && ok || bad "join: $(dur "$M/j.mp3") $OUT"
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/a.m4a")" --op media.audio
  [ $RC -eq 0 ] && [ -z "$("$FFDIR/ffprobe.exe" -v error -select_streams v -show_entries stream=codec_type -of csv=p=0 "$M/a.m4a")" ] && ok || bad "audio only: $OUT"
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/m.mp4")" --op media.mute
  [ $RC -eq 0 ] && [ -z "$("$FFDIR/ffprobe.exe" -v error -select_streams a -show_entries stream=codec_type -of csv=p=0 "$M/m.mp4")" ] && ok || bad "mute: $OUT"
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/c.mp4")" --op media.crop --option crop=10,10,160,120
  [ "$("$FFDIR/ffprobe.exe" -v error -select_streams v -show_entries stream=width,height -of csv=p=0 "$M/c.mp4" | tr -d '\r')" = "160,120" ] && ok || bad "video crop: $OUT"
  roda --convert "$(win "$CORPUS/clip.wmv")" "$(win "$M/s.mp4")" --op media.compress
  [ $RC -eq 0 ] && [ "$(stat -c %s "$M/s.mp4")" -lt "$(stat -c %s "$CORPUS/clip.wmv")" ] && ok || bad "video compress: $OUT"
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/s2.mp4")" --op media.compress
  { [ $RC -eq 0 ] && [ "$(stat -c %s "$M/s2.mp4")" -lt "$(stat -c %s "$CORPUS/clip.mp4")" ]; } || { [ $RC -eq 1 ] && [[ "$OUT" == *"already well compressed"* ]] && [ ! -e "$M/s2.mp4" ]; } && ok || bad "compress grew a file: $OUT"
  roda --convert "$(win "$CORPUS/tone.wav")" "$(win "$M/s.m4a")" --op media.compress; [ $RC -eq 0 ] && ok || bad "audio compress: $OUT"
  roda --convert "$(win "$CORPUS/clip.mp4")" "$(win "$M/r.mp4")" --op media.720
  [ $RC -eq 1 ] && [[ "$OUT" == *"already 240 pixels"* ]] && ok || bad "720p of a small video: $OUT"
  roda --convert "$(win "$CORPUS/tone.mp3")" "$(win "$M/x.mp4")" --op to.mp4
  [ $RC -eq 1 ] && ok || bad "a picture-less MP4: $OUT"
  roda --convert "$(win "$CORPUS/garbage.jpg")" "$(win "$M/g.mp3")" --op to.mp3
  [ $RC -eq 1 ] && [[ "$OUT" == error:* ]] && [ ! -e "$M/g.mp3" ] && ok || bad "garbage to MP3: $OUT"
else
  echo "(no engines/ffmpeg next to the exe: audio and video tests skipped)"
fi

# ---------------------------------------------------------------- broken files fail cleanly
for in in empty.png truncated.png garbage.jpg; do
  roda --convert "$(win "$CORPUS/$in")" "$(win "$WORK/broken-$in.webp")"
  [ $RC -eq 1 ] && [[ "$OUT" == error:* ]] && [ ! -e "$WORK/broken-$in.webp" ] && ok || bad "$in: rc=$RC '$OUT'"
done

# ---------------------------------------------------------------- never over the source, names stay unique
cp "$CORPUS/photo.png" "$WORK/same.png"
roda --convert "$(win "$WORK/same.png")" "$(win "$WORK/same.png")"
[ $RC -ne 0 ] && cmp -s "$CORPUS/photo.png" "$WORK/same.png" && ok || bad "the source was overwritten: $OUT"

# A path longer than 260 characters.
long="$WORK"; for i in $(seq 1 12); do long="$long/a-rather-long-folder-name-$i"; done
mkdir -p "$long" && cp "$CORPUS/photo.png" "$long/photo.png"
roda --convert "$(win "$long/photo.png")" "$(win "$long/photo.webp")"
[ $RC -eq 0 ] && [ -s "$long/photo.webp" ] && ok || bad "long path (${#long} chars): $OUT"

# Nothing half-written left anywhere.
[ -z "$(find "$WORK" -name '*.part')" ] && ok || bad "partial files left: $(find "$WORK" -name '*.part')"

echo "passed $pass, failed $fail"
[ $fail -eq 0 ]
