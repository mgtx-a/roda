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
