# Roda

Roda (Malay for "wheel") is a portable, offline file converter for Windows 10 and 11. Drag a file, hold
Shift, Ctrl or Alt, and a wheel of options opens under the pointer. Drop the file on an option and it is
converted or acted on, locally.

> **Status: Phase 3, awaiting hand-tests.** Pictures, PDFs, documents, spreadsheets, subtitles,
> archives and file actions work. Audio and video come in Phase 4.

## Download
Not released yet. Two builds, for x64 and ARM64, neither needing an installer, admin rights or a runtime:

- **Full** (a zip): `Roda.exe` plus `engines\pdfium.dll` for PDFs. Unzip it anywhere, including a USB
  stick; delete the folder to remove it.
- **Lite**: `Roda.exe` alone, everything except PDFs (the PDF options are simply not shown).

## How to use
1. Start `Roda.exe`. A wheel icon appears in the tray.
2. Start dragging one or more files (from Explorer, the desktop, a browser's downloads, ...).
3. While dragging, press **Shift** (Convert), **Ctrl** (Tools) or **Alt** (Actions). The wheel opens.
   Pressing another of these keys switches wheels. The dots in the centre show which one is open.
4. Release the mouse button on an option. To cancel, press **Esc**, drop on the centre of the wheel, or
   release outside the wheel (the files then go wherever you dropped them, as usual).
5. A card in the bottom-right corner shows the progress, then **Saved as photo.webp** with **Open** and
   **Show in folder**. Click the cross on a running card to cancel it. Failures say what went wrong.

Without dragging: select files in Explorer or on the desktop and press **Ctrl+Alt+R**. The wheel opens
with the keyboard in charge: arrows choose, **Tab** switches wheel, **Enter** applies, **Esc** cancels.

Keys you already hold when the drag starts (Shift+drag to move, say) don't open the wheel: press the key
once the drag is under way. The wheel doesn't open while a full screen app (a game, a video, a
presentation) is in front.

### What's on the wheels
Only the options that work for **every** dropped item are shown; folders and mixed selections get the
Actions wheel.

- **Convert** (pictures): JPG, PNG, WEBP, PDF, TIFF, BMP, ICO.
  Reads JPG, PNG, WebP, GIF (first frame), BMP, TIFF (first page), ICO, SVG, and HEIC/AVIF when
  Microsoft's free "HEIF Image Extensions" / "AV1 Video Extension" are installed.
- **Tools** (pictures): Compress (a quality slider), 50%, 1920 px, 1080 px, Size… (width/height),
  Crop (drag a rectangle; free, 1:1, 4:3, 16:9), Rotate, Flip, Strip info, Merge PDF.
  Archives (ZIP, TAR, TAR.GZ, GZ): Extract, into a new folder named after the archive.
- **PDFs** (Full build). Convert: PNG and JPG (every page at 300 DPI: one file for a one-page PDF,
  otherwise a folder named after the PDF), TXT, MD (headings, lists, bold and italic recovered from the
  fonts), DOCX (the text, plus the PDF's pictures), EPUB, PPTX (one slide per page, as a picture).
  Tools: Split (a PDF per page), Images (the pictures inside, JPEGs as they are), Rotate (every page a
  quarter turn), Merge PDF (PDFs and pictures, in the order dropped). Float shows the first page.
- **Documents**: DOCX → HTML, MD, TXT, EPUB. Markdown → HTML, DOCX, TXT, EPUB. HTML → MD, TXT, DOCX.
  RTF → TXT, HTML, MD, DOCX. TXT → DOCX, EPUB, HTML. Headings, paragraphs, bold, italic, links, lists,
  tables, quotes, code and pictures carry over where both formats have them.
- **Spreadsheets**: XLSX, ODS, CSV, TSV and old XLS → XLSX, ODS, CSV, TSV. Values travel (text, numbers,
  true/false, dates; formulas as their last result), formatting doesn't. Every sheet is kept; to CSV or
  TSV each sheet becomes its own file in a folder when there are several. CSVs with semicolons (as Excel
  writes them in many countries) are recognised. CSV comes out as UTF-8 with the mark Excel needs.
- **Subtitles**: SRT ↔ VTT, both → TXT (just the words).
- **Office files to PDF**, when Microsoft Office is installed: Word (DOCX, DOC, RTF), Excel (XLSX, XLS,
  ODS) and PowerPoint (PPTX, PPT) make the PDF themselves, so it looks exactly as in Office. Office runs
  hidden and opens the file read-only; the option doesn't appear on PCs without that Office app.
- **Actions** (anything): Copy to…, Move to… (with Explorer's own progress, conflict questions and
  undo), the last three "Copy to" folders, Copy path, Terminal, Float (a picture or a PDF's first page
  that stays on top: drag to move, mouse wheel or corners to resize, double-click to open, Esc to
  close), ZIP, TAR.GZ, TAR.

Results go next to the original, under a new name (`photo.webp`, `photo (1).webp`, `photo compressed.jpg`,
...). The original is never changed or replaced. Unfinished results are written as `*.part` and only
renamed when complete, so a cancelled or failed job leaves nothing behind.

Good to know:
- WebP from PNG, GIF, BMP, ICO or SVG is lossless; from photos it is lossy (quality 82).
- Photo metadata (EXIF, colour profile) is kept when a JPEG stays a JPEG (Compress, resize, rotate...)
  unless `keepMetadata` is off; other conversions drop it. **Strip info** removes it from JPEG, PNG and
  WebP without re-encoding the picture.
- Pictures stored sideways (EXIF orientation) come out upright.
- Settings for now: tray → *Settings (edit settings.json)*: `jpegQuality`, `webpQuality`,
  `keepMetadata`, `outputFolder` (empty: next to each file), trigger keys, `hotkeyMods`/`hotkeyKey`.

Known limits: a drag from an app running as administrator can't open the wheel (Windows does not let a
normal app receive drops from an elevated one); don't run Roda as administrator to get around it.
A PDF made of scanned pages has no text to extract (no OCR); password-protected PDFs are refused with a
plain message. Not yet: RAR extraction, AVIF/HEIC output, Share, PDF to XLSX, PDF compression, DOCX →
PDF without Office installed, audio and video (Phase 4).

## Privacy & permissions
Roda makes **no network connections**: no telemetry, no update checks, nothing is uploaded.
What it touches:

- **Mouse:** raw mouse input (button and movement) is used to notice that a drag has started. Roda does
  **not** install keyboard or mouse hooks.
- **Other windows:** when a drag starts, Roda asks Windows which window holds the mouse capture (its class
  and program name go in the log), and whether a full screen app is in front, in which case it stays out
  of the way. For the hotkey it asks the Explorer window in front which files are selected.
- **Keys:** Shift, Ctrl, Alt and Esc are read only while a file drag is in progress; the hotkey is
  registered with Windows (`RegisterHotKey`).
- **Files you drop:** read to convert them, by a separate `Roda.exe --worker` process per job (at
  below-normal priority), so a damaged file can only stop its own worker. Results are written next to
  the original, or in `outputFolder` if you set one. Copy to / Move to use Explorer's own file engine.
  PDFs are read by `engines\pdfium.dll`, which only workers load, by its full path, when a PDF is opened.
- **Microsoft Office** (only for "to PDF" of Office files, only if installed): the worker starts Word,
  Excel or PowerPoint invisibly, opens the file read-only, saves the PDF and closes it. It quits Office
  afterwards unless other files are open in it; cancelling the job ends an Office it started. Whether
  Office is installed is read from the registry (nothing is written).
- **Clipboard:** written only by *Copy path*.
- **Programs started:** Windows Terminal (or Command Prompt) for *Terminal*; the default app for a result
  when you click *Open*; Explorer for *Show in folder*.
- **Settings and log:** `settings.json` and `log.txt` (kept under 200 KB) in a `data` folder next to
  `Roda.exe`, or in `%APPDATA%\Roda` if that folder is read-only. The log says which.
- **Temporary files:** `%TEMP%\Roda\<process id>` (previews for Float and Crop), deleted on exit and
  cleaned up at the next start.
- **Start with Windows** (tray menu): a `Roda.lnk` shortcut in your Startup folder, removed when you
  untick it.
- **Registry:** nothing is written. The light/dark theme setting is only read.

## Command line
For scripts and tests (`start /wait` from cmd, as Roda.exe is a Windows app):

    Roda.exe --convert photo.png photo.webp
    Roda.exe --convert a.jpg b.png merged.pdf
    Roda.exe --convert photo.jpg small.jpg --op tool.compress --option q=60
    Roda.exe --convert photo.jpg out.jpg --op tool.resize --option w=800
    Roda.exe --convert folder notes.txt pack.zip
    Roda.exe --convert pack.zip outdir\
    Roda.exe --convert report.pdf report.docx
    Roda.exe --convert report.pdf outdir\ --op to.png --option dpi=150
    Roda.exe --convert report.pdf outdir\ --op pdf.split
    Roda.exe --convert book.xlsx book.csv
    Roda.exe --info photo.webp

It exits with 0 on success, 1 when the file could not be converted (with an `error:` line), 2 for bad
arguments.

## Build
Needs Zig 0.17 (C++20, MinGW-style target). The script defaults to the toolchain path on the author's PC;
set `ZIG=zig` to use the one on your `PATH`.

    ./build.sh [outdir] [x64|arm64|all]    # -> out/x64/Roda.exe, out/arm64/Roda.exe
    bash tests/run_tests.sh out/x64/Roda.exe

`tests/run_tests.sh` converts every sample in `tests/corpus` (pictures, PDFs, documents, spreadsheets,
subtitles, archives) to every format and through every tool, and
checks the results decode again with the right size; broken files must fail cleanly. Trigger self-test:
`tests/dragtest.cpp` plays real drags with injected input (it moves the pointer); results of the Phase 1
run are in [tests/phase1-results.md](tests/phase1-results.md).

Licences of third-party code: [THIRD_PARTY.md](THIRD_PARTY.md). Portions of this software are copyright
© The FreeType Project (www.freetype.org). All rights reserved.
