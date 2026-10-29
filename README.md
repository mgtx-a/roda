# Roda

Roda (Malay for "wheel") changes files from one type to another on your Windows PC. Photo to PDF,
video to MP4, Word to PDF, and many more.

You drag a file, press a key, and a wheel of choices pops up under your mouse. Drop the file on a
choice and Roda does the rest. Everything happens on your own computer: nothing is uploaded.

Works on Windows 10 and 11. No installing, no admin rights, no account.

> Roda is still being tested and has no official release yet.

## Get it
There are two versions:

- **Full** (a zip, about 23 MB once unzipped): does everything, including PDFs, audio and video.
- **Lite** (one file, under 2 MB): everything except PDFs, audio and video.

Unzip it anywhere you like, even a USB stick, and double-click `Roda.exe`. To remove Roda, just delete
the folder.

Until the first release, test builds are on the repository's **Actions** tab.

## How to use it
1. Double-click `Roda.exe`. A small wheel icon appears near the clock.
2. Start dragging a file, as if you were moving it.
3. While still holding the mouse button, press one of these keys:
   - **Shift**: change the file type (Convert)
   - **Ctrl**: edit the file: smaller, resized, cropped, rotated... (Tools)
   - **Alt**: copy, move, zip, and so on (Actions)
4. A wheel opens. Let go of the mouse on the choice you want.
5. A small card in the bottom-right corner shows the progress. When it's done, click **Open** or
   **Show in folder**.

The new file is saved next to the original, with the same name: `photo.jpg` becomes `photo.png`.
**Your original file is never changed.**

To cancel: press **Esc**, or let go in the middle of the wheel. To stop a job that's running, click the
**×** on its card.

The first time you start Roda, a short tour shows you how it works.

### Other ways to open the wheel
- **Keyboard:** select files in File Explorer and press **Ctrl+Alt+R**. Use the arrow keys to choose,
  **Enter** to go, **Esc** to cancel.
- **Right-click:** turn on *Right-click in Explorer* in Settings. Files then get a **Roda…** item when
  you right-click them. (On Windows 11, it's under *Show more options*.)
- **From a ZIP or an Outlook email:** drag a file straight out of an open ZIP folder or an email.
  The result is saved in your **Downloads** folder.

### Tips
- Press the key **after** you start dragging, not before.
- The wheel only shows choices that work for the files you're dragging.
- The wheel won't pop up while a game or full-screen video is in front.

## What it can do

| Files | Change into | Tools |
|---|---|---|
| **Photos** (JPG, PNG, WebP, GIF, BMP, TIFF, ICO, SVG, HEIC, AVIF) | JPG, PNG, WebP, PDF, TIFF, BMP, ICO | Make smaller, resize, crop, rotate, flip, remove photo info, join into one PDF |
| **PDFs** \* | Pictures (PNG, JPG), text, Word (DOCX), ebook (EPUB), PowerPoint (PPTX) | Remove pages, split into pages, take out the pictures, rotate, join PDFs |
| **Documents** (Word DOCX, Markdown, HTML, RTF, TXT) | Each other, and ebook (EPUB) | |
| **Word, Excel, PowerPoint** | PDF (only if Microsoft Office is on your PC) | |
| **Spreadsheets** (Excel XLSX and XLS, ODS, CSV, TSV) | XLSX, ODS, CSV, TSV | |
| **Subtitles** (SRT, VTT) | Each other, or plain text | |
| **Audio** \* (MP3, M4A, WAV, FLAC, OGG, Opus, AIFF, WMA) | Each other | Cut a part, make smaller, join several |
| **Video** \* (MP4, MOV, MKV, WebM, AVI, WMV, GIF) | Each other, or GIF, or just the sound | Cut a part, make smaller, mute, 1080p / 720p / 480p, crop |
| **ZIP and TAR files** | | Unpack into a folder |
| **Anything** | | Copy to, Move to, make a ZIP, copy the file's path, open a terminal here, pin a picture on top of your screen |

\* Full version only.

### Settings
Right-click the wheel icon near the clock and choose **Settings**. There you can change:
- the keys that open the wheel, and the Ctrl+Alt+R shortcut
- where new files are saved (next to the original, or always in one folder)
- picture quality
- light or dark look
- start Roda with Windows
- the right-click item in File Explorer

Changes work straight away.

### Things it can't do (yet)
- Read text from scanned PDFs, or open password-protected PDFs.
- Turn Word files into PDF without Microsoft Office.
- Open RAR files.
- Save pictures as AVIF or HEIC (it can open them, but not save them).
- Work with touch screens or pens.
- Other languages besides English.
- Take files dragged from a program that runs as administrator (Windows blocks this).

## Privacy
**Roda never connects to the internet.** It has no ads, no tracking and no update checks, and your
files never leave your computer.

What Roda does on your PC, in plain words:

- **Mouse and keys:** it notices when you start dragging a file, and only then checks for
  Shift, Ctrl, Alt and Esc. It doesn't record your typing.
- **Your files:** it reads the files you drop on the wheel and saves the new file next to them (or in
  the folder you chose). Each job runs as its own small helper process, so one broken file can't crash
  Roda.
- **Audio, video and PDFs** are handled by helper files in the `engines` folder (FFmpeg and PDFium).
  They run on your PC, can only read and write files, and contain no internet code.
- **Microsoft Office:** for Word/Excel/PowerPoint to PDF, Roda asks your own Office to do it, hidden in
  the background. It opens your file read-only.
- **Settings and log:** saved in a `data` folder next to `Roda.exe` (or in `%APPDATA%\Roda` if that
  folder can't be written to). The log is a small text file that says what Roda did.
- **Temporary files:** previews, and files dragged out of ZIPs or emails, go in `%TEMP%\Roda` and are
  deleted when Roda closes.
- **Start with Windows:** if you turn it on, Roda puts a shortcut in your Startup folder. Turning it off
  removes the shortcut.
- **Registry:** Roda writes nothing there, unless you turn on the right-click item in Settings
  (`HKEY_CURRENT_USER\Software\Classes\*\shell\Roda` and `...\Directory\shell\Roda`). Turning it off
  removes it. Roda only reads two things: your light/dark setting, and whether Office is installed.
- **Clipboard:** only used when you choose *Copy path*.

## For developers

### Command line
Roda can also convert from a script (use `start /wait` in cmd):

    Roda.exe --convert photo.png photo.webp
    Roda.exe --convert a.jpg b.png merged.pdf
    Roda.exe --convert photo.jpg small.jpg --op tool.compress --option q=60
    Roda.exe --convert photo.jpg out.jpg --op tool.resize --option w=800
    Roda.exe --convert folder notes.txt pack.zip
    Roda.exe --convert pack.zip outdir\
    Roda.exe --convert report.pdf report.docx
    Roda.exe --convert report.pdf outdir\ --op to.png --option dpi=150
    Roda.exe --convert report.pdf outdir\ --op pdf.split
    Roda.exe --convert report.pdf short.pdf --op pdf.remove --option "pages=2, 5-7"
    Roda.exe --convert book.xlsx book.csv
    Roda.exe --convert video.mkv video.mp4
    Roda.exe --convert video.mp4 clip.mp4 --op media.trim --option start=12 --option end=45
    Roda.exe --info photo.webp

It exits with 0 when it worked, 1 when the file couldn't be converted (with an `error:` line), and 2 for
wrong arguments.

### Build
Needs Zig 0.17. Set `ZIG=zig` to use the one on your `PATH`.

    ./build.sh [outdir] [x64|arm64|all]    # -> out/x64/Roda.exe, out/arm64/Roda.exe
    bash tests/run_tests.sh out/x64/Roda.exe

The FFmpeg in `engines\ffmpeg` is built separately on Linux by `tools/ffmpeg/build.sh` (GitHub Actions
does this); `build.sh` fetches it from the latest successful run. `NO_PDFIUM=1` and `NO_FFMPEG=1` make a
Lite build.

`tests/run_tests.sh` converts every sample in `tests/corpus` to every format and through every tool, and
checks the results. `tests/dragtest.cpp` tests the wheel with real drags (it moves the mouse); results
of the first run are in [tests/phase1-results.md](tests/phase1-results.md).

## Licence
Roda is free software under the [MIT licence](LICENSE). The licences of the code it uses are in
[THIRD_PARTY.md](THIRD_PARTY.md). Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved.
