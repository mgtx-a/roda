# Roda: build guide

Roda (Malay for "wheel") is a portable, lightweight, offline file converter for Windows.
You drag a file, hold a modifier key, and a wheel of options opens under the pointer;
you drop the file on an option and it is converted, processed or acted on, locally.

The interaction model follows Tangerine for Mac (https://tangerineformac.com/). The
look is our own: do **not** copy Tangerine's name, icon, artwork, colours or exact visuals.

This document is the full brief. Read it all before writing code, then work phase by phase.

---

## 1. Goals and hard constraints

1. **Portable.** No installer, no admin rights, nothing required in the registry. The app
   runs from any folder, including a USB stick. Deleting the folder removes it completely.
2. **Runs on any Windows 10 / 11 PC with no issue.** Do not depend on anything a PC might
   lack:
   - no Microsoft Store codec extensions (HEIC, HEVC, AV1, WebP, VP9),
   - no Media Foundation (Windows "N" editions don't have it),
   - no .NET, no VC++ redistributable, no installed Office, no internet connection.
   Everything the app needs ships in its folder. Optional: Office-only features may light
   up when Office *is* installed (section 7.6).
3. **Lightweight.** C++20, plain Win32 + Direct2D + DirectWrite + WIC. No UI framework,
   no Electron, no Qt, no .NET. Targets:
   - idle memory (private working set) under 5 MB,
   - idle CPU 0% (no polling when nothing is happening),
   - core exe under 2 MB; full portable folder under ~30 MB.
4. **Offline and private.** No network code at all: no telemetry, no update checks, no
   uploads. The README must say so and list exactly what the app touches (section 12).
5. **Never destroys data.** Never overwrites or deletes the source file. Output goes next
   to the source under a new, unique name.
6. **Not mistaken for malware.** No global keyboard or mouse hooks (section 5.1). No code
   injection, no hidden processes, no self-modifying behaviour, no packers (UPX etc.).
7. **Architectures:** x64 and ARM64 builds. 32-bit and Windows 7/8.1 are out of scope
   (the core may happen to work on 8.1; don't promise it).

## 2. Reuse the Ampaian codebase

Ampaian is the user's existing app built with exactly this stack. Reuse its code and
conventions instead of starting from zero:

- Local: `C:\Users\piani\Desktop\Dev Projects\sangkutss\Ampaian-Native`
- GitHub: https://github.com/mgtx-a/ampaian

| Ampaian file | What to reuse |
|---|---|
| `src/common.h`, `src/util.cpp` | Includes, string helpers, paths, `Log()`, settings JSON load/save, `Now()`, `After()` timers, monitors, DPI scale |
| `src/gfx.h/.cpp` | Direct2D/DirectWrite/WIC setup, `Surface` (layered-window drawing via DIB + `UpdateLayeredWindow`), shadows, text, image loading (`LoadMips`) |
| `src/anim.h/.cpp` | Springs and easing curves: use them for the wheel opening and segment highlights |
| `src/dragdrop.h/.cpp` | OLE drag source/target plumbing (`IDropTarget`, `IDataObject`, drag images) |
| `src/panel.cpp` | Pattern for a topmost, no-activate, layered, click-through-when-idle window |
| `src/app.cpp` | Single-instance mutex, tray icon + menu, the message loop with `MsgWaitForMultipleObjectsEx`, frame pacing, UI-thread posting (`PostToUi`) |
| `src/flight.cpp` | Full-screen overlay animation: basis for the "Float" action's always-on-top card |
| `build.sh` | Zig C++ build (`zig c++ -target x86_64-windows-gnu ...`), honours `ZIG=` override |
| `.github/workflows/build.yml` | CI build on `windows-latest`, provenance attestation, release on `v*` tags |
| `.gitattributes` | Keep `*.sh` in LF so CI's bash can run them |

Read those files before designing the equivalents in Roda. Match their style: short
functions, comments that explain *why*, plain-English log lines, no exceptions, `Com<T>`
smart pointers for COM.

## 3. What the user sees

### 3.1 The trigger
1. The user starts dragging one or more files or folders from anywhere: Explorer, the
   desktop, a browser's downloads bar, an email attachment, another app.
2. While dragging, they press and hold a **trigger key**. A wheel opens centred on the
   pointer.
3. They move onto a segment (it highlights) and release the mouse button there: that
   option runs on the dragged files.
4. The wheel stays open after the key is released, until the user drops or cancels.
5. **Cancel:** press Esc, or release the mouse button outside the wheel (the files then
   go wherever they were dropped, as normal; Roda does nothing).
6. While the wheel is open, pressing a different trigger key switches to that wheel.

Default trigger keys (all configurable in settings):

| Key | Wheel |
|---|---|
| Shift | **Convert**: target formats for the dragged file type |
| Ctrl | **Tools**: compress, resize, crop, trim, split, merge, ... |
| Alt | **Actions**: Copy to, Move to, Copy path, Open terminal here, Float, Share |

Pressing the key during a drag also changes the Windows drop effect (Shift = move,
Ctrl = copy, Alt = link) for the window under the pointer. That is fine: once the wheel
is open the drop goes to Roda, which always copies (reads) and never moves the source.

### 3.2 The wheel
- A ring of segments around a centre disc. Up to 8 segments per ring; more options go on
  a second, outer ring. The centre disc shows the file type and count ("3 PNG files")
  and acts as Cancel.
- Each segment has a short label (e.g. `WEBP`, `Compress`, `Copy path`) and an optional
  icon. Draw icons as vector shapes in code or embed tiny PNGs as resources; no icon fonts
  that might be missing.
- Opening: spring scale from 0.85 to 1 with fade-in over ~150 ms. Hover: segment grows
  slightly and brightens. Drop: the chosen segment pulses, then the wheel shrinks into a
  progress card (3.3).
- Only options valid for **every** dragged file are shown. Mixed selections and folders
  still get the Actions wheel.
- Respect per-monitor DPI, light/dark mode (`AppsUseLightTheme` registry value, read-only)
  and reduced motion (`SPI_GETCLIENTAREAANIMATION`).
- The wheel must stay fully on-screen: clamp it near screen edges and corners.

### 3.3 Progress and results
- A small card near the pointer (later docked bottom-right) shows the file name, a
  progress bar and a Cancel button. Several jobs queue and show as a stack.
- Done: the card shows "Saved as photo.webp" with **Show in folder** and **Open**
  buttons, then fades after ~4 s. Clicking the card opens the output.
- Failure: the card says what went wrong in plain words ("This PDF is password
  protected") and the log has details.

### 3.4 Keyboard route (no drag)
- A global hotkey (`RegisterHotKey`, default Ctrl+Alt+R, configurable) opens the wheel
  for the files currently selected in the foreground Explorer window or on the desktop.
- Arrow keys move between segments, Enter applies, Tab switches wheel, Esc cancels.
- Tangerine uses plain Enter; on Windows Enter opens the file in Explorer and can't be
  intercepted without a keyboard hook, which is forbidden here. Use the hotkey.

### 3.5 Tray and settings
- A tray icon with: Settings, Open output folder, Pause Roda, Start with Windows
  (portable: creates/removes a shortcut in the user's Startup folder), Quit.
- Settings window (plain Win32 + Direct2D, same visual style): trigger keys, hotkey,
  output location (next to source / fixed folder), default quality per format,
  "keep metadata", theme, language later.
- First run: a short three-card tour like Ampaian's `guide.cpp`.

## 4. Architecture

```
Roda.exe  (one exe, two modes)
├── UI mode (default): tray, input watching, wheel, progress cards, job queue
│     └─ starts workers:  Roda.exe --worker  (one per job, max = CPU cores / 2)
└── Worker mode (--worker): reads one job, loads only the engines it needs,
      converts, reports progress, exits
```

- **Why a separate worker process:** decoders for PDF, video and images are the main
  crash and exploit surface. A malformed file can only kill its worker; the UI shows
  "could not convert" and carries on. Memory is freed completely when the worker exits,
  so idle memory stays tiny.
- **Job transport:** the UI writes a job as JSON to the worker's stdin and reads progress
  lines from its stdout (`{"progress":0.42}`, `{"done":"C:\\...\\out.webp"}`,
  `{"error":"..."}`). Use anonymous pipes from `CreateProcessW`.
- **Lifetime:** put workers in a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so
  they die if the UI exits or crashes. Cancel = `TerminateProcess` + delete the partial
  output.
- **Output safety:** a worker writes to `name.part` in the target folder, then renames it
  to the final name only on success. The final name is unique: `photo.webp`,
  `photo (1).webp`, ...
- **CLI mode for testing:** `Roda.exe --convert <in> <out> [--option k=v ...]` runs a
  conversion synchronously in the console and exits with 0 on success. All automated
  tests use this. It must work without the UI running.
- **Converter registry:** every converter registers a descriptor:

  ```cpp
  struct Converter {
      const wchar_t* id;            // "image.to.webp"
      Wheel wheel;                  // Convert / Tools / Actions
      const wchar_t* label;         // "WEBP"
      std::vector<FileKind> inputs; // which inputs it accepts
      const wchar_t* outExt;        // ".webp" (empty for actions)
      Engine needs;                 // which engine DLLs must be present
      bool needsOptions;            // opens a small options popup (crop, trim)
      bool (*run)(const Job&, Progress&);
  };
  ```

  The wheel is built from the registry for the dragged file kinds. Adding a format never
  touches the UI code.
- **Capability detection:** at startup the UI checks which optional engine DLLs are in
  its folder (`pdfium.dll`, FFmpeg DLLs, ...) and whether Office is installed. Options
  whose engine is missing are hidden, not shown greyed out. So one Roda.exe serves both
  the Lite and Full builds (section 9).
- **File kind detection:** by magic bytes first, extension second (a `.jpg` that is
  really a PNG still works).

## 5. The trigger: how to implement it

This is the riskiest part of the project. Build it first (Phase 1) and validate it
everywhere before writing any converter.

### 5.1 Watching the mouse without hooks
- Register for raw mouse input on a message-only window:
  `RegisterRawInputDevices` with usage page 1, usage 2, `RIDEV_INPUTSINK`. `WM_INPUT`
  then delivers `RI_MOUSE_LEFT_BUTTON_DOWN` / `UP` and movement system-wide without a
  hook.
- Read modifier keys with `GetAsyncKeyState`, and **only** while a left-button drag is in
  progress. Poll them at most every 16 ms in that state, never at idle.
- Never use `SetWindowsHookEx` (`WH_MOUSE_LL`, `WH_KEYBOARD_LL`). Antivirus tools flag
  unsigned apps with global hooks as keyloggers, and Windows silently removes slow
  low-level hooks.

### 5.2 Knowing that a *file* drag is in progress
A mouse button held while moving is not enough: Shift + drag also selects text in editors
and Ctrl + drag is used everywhere. Roda must react only to real OLE drags carrying files.

Approach (validate it in Phase 1):
1. On left button down, remember the point. When the pointer moves beyond
   `SM_CXDRAG`/`SM_CYDRAG` with the button still down, a drag *may* have started.
2. Show a tiny **probe** window (for example 24×24 px, layered, per-pixel alpha 1/255 so
   it's invisible but still hit-testable, `WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
   WS_EX_NOACTIVATE`) centred under the pointer, and keep it under the pointer on each
   raw-input move for up to ~400 ms. It is registered with `RegisterDragDrop`.
3. If an OLE drag is in progress, `DoDragDrop` in the source process hit-tests the window
   under the pointer and calls the probe's `IDropTarget::DragEnter`. In `DragEnter`,
   check the `IDataObject` for `CF_HDROP` (and later `CFSTR_FILEDESCRIPTORW` for virtual
   files). If present: a file drag is in progress. Read and remember the paths now.
4. **Immediately** move the probe away from the pointer (hide it) and return
   `DROPEFFECT_NONE`, so the drag goes on to the user's real target as if Roda weren't
   there.
5. If no `DragEnter` arrives within ~400 ms, it isn't an OLE file drag (text selection,
   window move, game, drawing app). Hide the probe and ignore modifiers until the button
   is released.
6. While a file drag is known to be in progress and the button is still down: when a
   trigger key goes down, show the wheel (a full-size layered drop-target window) centred
   on the pointer. The next pointer move makes `DoDragDrop` send `DragEnter` to the wheel.
7. Wheel `DragOver`: hit-test the segment under the pointer, highlight it, and return
   `DROPEFFECT_COPY` over a segment or `DROPEFFECT_NONE` over empty space. Wheel `Drop`:
   run the segment's option on the paths from the `IDataObject`.
8. Left button up anywhere (raw input) ends the drag state: hide the wheel if still open.

Details that matter:
- **Hit-testing layered windows:** pixels with alpha 0 are click-through, *including for
  drag and drop*. Paint every pixel of the wheel's disc with alpha ≥ 1, or the drop falls
  through gaps between segments.
- **Ignore our own drags:** when Roda itself starts a drag (from a result card or a Float
  window), skip the probe.
- **Elevation (UIPI):** a drag from an app running as administrator can't drop on a
  normal-level Roda. Don't try to work around it; document it in the README.
- **Virtual files** (Outlook attachments, files inside zip folders in Explorer) arrive as
  `CFSTR_FILEDESCRIPTORW` + `CFSTR_FILECONTENTS`, not `CF_HDROP`. Phase 6: copy them to a
  temp folder first, then convert.
- **Fallback if the probe proves unreliable** in Phase 1: show the wheel when the trigger
  key is pressed during *any* button-held drag, and dismiss it if no `DragEnter` with
  files arrives within 150 ms of the next pointer move. Write down what you measured.

### 5.3 Phase 1 test matrix
Run the trigger against every row. All must pass before Phase 2.

| Drag source | Expected |
|---|---|
| Explorer window, one file | Wheel opens; drop runs the option |
| Explorer, 20 mixed files + a folder | Actions wheel only, or common options |
| Desktop icons | Works |
| Chrome, Edge, Firefox downloads | Works |
| Text selection in Notepad / Word / browser with Shift held | Wheel must **not** open |
| Dragging a window by its title bar with Ctrl/Shift | Wheel must **not** open |
| Normal file drag with no trigger key | Roda invisible; the drop lands normally |
| Explorer running as admin | No wheel; nothing breaks |
| 100%, 150%, 200% scaling; two monitors with different DPI | Wheel sharp and on the right screen |
| Windows 10 22H2 and Windows 11 24H2 | All of the above |

## 6. Conversion engines

Principle: Windows' own components where they exist on **every** edition; small,
permissively licensed libraries next; heavy engines only as optional DLLs.

| Engine | Use | Licence | How it ships |
|---|---|---|---|
| WIC (built in) | Decode/encode PNG, JPEG, BMP, GIF, TIFF, ICO | Windows | Built in |
| libwebp | WebP decode + encode | BSD | Static |
| libheif + libde265 | HEIC decode | LGPL | DLLs (LGPL: keep replaceable) |
| libavif + dav1d | AVIF decode | BSD | Static |
| libavif + an AV1 encoder (aom or SVT-AV1) | AVIF encode | BSD | DLL, optional (it's a few MB) |
| lunasvg | SVG input (render to bitmap) | MIT | Static |
| Own PDF writer | Images → PDF (embed JPEG with DCTDecode / PNG with Flate) | — | Static, small |
| PDFium | PDF render, text extraction, page import/merge/split | BSD/Apache | `pdfium.dll` (prebuilt: bblanchon/pdfium-binaries), x64 + ARM64 |
| FFmpeg libav* (LGPL build only) | All audio/video decode; encode AAC, FLAC, ALAC, WAV, AIFF, WMA, MP3 (libmp3lame), Opus (libopus), Vorbis (libvorbis), VP8/VP9 (libvpx), MPEG-4, WMV, GIF | LGPL + BSD libs | DLLs. Dev: BtbN/FFmpeg-Builds `lgpl-shared`. Release: own trimmed build in CI |
| H.264 encoder | MP4/MOV output | — | Media Foundation encoder when present; else OpenH264 (BSD source; see 13.3) |
| md4c | Markdown → HTML | MIT | Static |
| pugixml | XML for DOCX/XLSX/PPTX/ODS/EPUB | MIT | Static |
| miniz | ZIP for Office/EPUB containers | MIT | Static |
| libarchive (+ zlib, bzip2, xz) | ZIP, TAR, GZIP read/write; RAR read | BSD | Static or DLL |
| libxls | XLS (BIFF) read | BSD | Static |
| Office via COM (optional) | DOCX/PPTX/XLSX → PDF with full fidelity | — | Only when Office is installed |

**Never** link GPL code (x264, x265, FFmpeg `--enable-gpl`), because it would force the
whole app under GPL. Record every library, version and licence in `THIRD_PARTY.md` and
ship its licence text in a `licenses` folder.

Building C libraries with Zig: Zig compiles C as well as C++, so static libraries like
libwebp, dav1d, md4c, pugixml, miniz, libarchive and lunasvg can be built from source in
`build.sh`; vendor them under `third_party/` at pinned versions. PDFium and FFmpeg come as
prebuilt DLLs with a C API and are loaded with `LoadLibraryW`/`GetProcAddress` (only in
workers, only when needed), so the toolchain difference doesn't matter.

## 7. Feature list

Ship it in the phase order (section 8). "Later" items are fine to leave out of 1.0.

### 7.1 Images
- Input: JPG, PNG, WebP, HEIC, TIFF, AVIF, BMP, GIF (first frame), ICO, SVG.
- Convert to: JPG, PNG, WebP, TIFF, AVIF, BMP, ICO, PDF. HEIC output only if a non-GPL
  encoder is available (otherwise leave it out; don't ship x265).
- Tools: compress (quality slider, target size later), resize (presets: 50%, 1920px,
  1080px, custom), crop (options popup with a draggable rectangle), rotate, flip, strip
  metadata, merge into one PDF.
- Keep EXIF orientation correct; keep or strip metadata per setting; keep transparency
  where the target supports it (PNG, WebP, AVIF, TIFF, ICO), otherwise fill white.

### 7.2 Audio
- Input/output: MP3, M4A (AAC/ALAC), WAV, FLAC, OGG (Vorbis), Opus, AIFF, WMA.
- Tools: trim (start/end popup), compress (bitrate), merge (concatenate in drag order),
  normalise volume (later).

### 7.3 Video
- Input: MP4, MOV, MKV, WebM, AVI, WMV, GIF.
- Convert to: MP4 (H.264 + AAC), MOV, MKV, WebM (VP9 + Opus), AVI (MPEG-4), WMV, GIF
  (palette-generated), MP3 (audio only).
- Tools: compress (quality preset), trim, extract audio, mute, resize (1080p/720p/480p),
  merge (later), extract frame as image (later).
- Use stream copy (no re-encode) whenever only the container changes and the codecs fit.

### 7.4 PDF
- To JPG / PNG: every page at 300 DPI (setting), one file per page in a folder named
  after the PDF.
- To TXT and Markdown: PDFium text extraction; Markdown headings from font-size jumps.
- To DOCX: text paragraphs per page plus page images where there are graphics.
- To PPTX: one slide per page, each slide the page rendered as an image (simple and
  faithful).
- To EPUB: text flow.
- To XLSX: experimental (text positions grouped into a grid); leave it out of 1.0 if
  results are poor.
- Tools: merge (several PDFs in drag order), split (one PDF per page or by ranges),
  extract images, rotate pages, compress (re-render image-heavy pages at lower DPI; later).

### 7.5 Documents, spreadsheets, subtitles, archives
- Documents: DOCX → HTML, Markdown, TXT, EPUB. Markdown → HTML, DOCX, TXT. HTML →
  Markdown, TXT. RTF → TXT, HTML. TXT → DOCX, EPUB. (Apple Pages: skip, it's Mac-only.)
- Spreadsheets: XLSX ↔ ODS ↔ CSV ↔ TSV; XLS input. First sheet by default; a multi-sheet
  workbook to CSV writes one file per sheet.
- Subtitles: SRT ↔ VTT, both → TXT.
- Archives: Compress to ZIP (any files/folders), TAR, TAR.GZ. Extract ZIP, TAR, GZIP,
  RAR. Extract into a new folder named after the archive.

### 7.6 Office-only conversions (when Office is installed)
Detect with `CLSIDFromProgID(L"Word.Application")` etc. When present, add:
- DOCX/RTF → PDF: `Word.Application` → `Documents.Open` → `ExportAsFixedFormat`.
- PPTX/PPT → PDF, JPG, PNG: `PowerPoint.Application` → `SaveAs` / `Export`.
- XLSX/XLS → PDF: `Excel.Application` → `ExportAsFixedFormat`.
Run these in the worker process, with Office invisible, and always call `Quit`.

### 7.7 Actions (no conversion)
- **Copy to… / Move to…:** folder picker (`IFileOpenDialog` with `FOS_PICKFOLDERS`, with
  recent folders as extra segments), then `IFileOperation` so Explorer's progress, undo
  and conflict dialogs work.
- **Copy path:** full paths, one per line, quoted if they contain spaces, as
  `CF_UNICODETEXT`.
- **Open terminal here:** `wt.exe -d "<folder>"` if Windows Terminal exists, otherwise
  `cmd.exe` with that working directory.
- **Float:** an always-on-top, movable, resizable preview card of an image or PDF page
  (reuse Ampaian's card drawing). Esc or the close cross closes it.
- **Share:** the Windows share window (Nearby Sharing, email, apps) through
  `IDataTransferManagerInterop::ShowShareUIForWindow`. Check the header and interface
  availability with the chosen toolchain; skip the action if it's unavailable.
- Actions work for folders, mixed selections and unknown file types.

## 8. Phases and acceptance criteria

Work in this order. At the end of each phase: build, run the tests, update the README,
commit, and tell the user what to try by hand. Don't start the next phase until the user
has confirmed the hand-tests.

### Phase 0: skeleton (≈1 day)
- Repo layout (section 10), `build.sh` for x64 + ARM64, CI workflow copied from Ampaian
  and adapted, `.gitattributes`, README with the privacy section, `THIRD_PARTY.md`.
- Tray icon, single instance, settings load/save (portable rule, section 9), log file.
- Done when: CI builds both architectures; the exe starts, shows the tray icon, quits
  cleanly; idle memory < 5 MB and idle CPU 0%.

### Phase 1: trigger prototype (≈1 week): the go/no-go phase
- Raw input, the probe, the wheel window with placeholder segments that just log the
  chosen segment and the dropped paths.
- Done when: every row of the 5.3 test matrix passes on Windows 10 and 11, and the wheel
  appears within one frame (≤16 ms) of the key press. Report measured results to the user.

### Phase 2: wheel UI + images + actions = first release (Lite)
- Final wheel visuals and animations, keyboard route, progress cards, job queue, worker
  process, CLI mode, output naming.
- Images (7.1, minus HEIC/AVIF encode), all actions (7.7), archives (7.5).
- Done when: CLI tests pass for every image pair; hand-tests pass; release `v0.1.0`.

### Phase 3: PDF and documents
- PDFium integration (7.4), documents, spreadsheets, subtitles (7.5).
- Done when: CLI tests pass on the sample corpus, including encrypted and broken PDFs
  (which must fail cleanly, with a plain error message).

### Phase 4: audio and video (Full build)
- FFmpeg integration (7.2, 7.3), trim/crop option popups, cancel mid-conversion.
- Trimmed FFmpeg build in CI (only the needed demuxers, muxers, decoders, encoders,
  filters: target < 15 MB for all DLLs).
- Done when: every audio/video pair in the corpus converts; cancelling leaves no partial
  files; a 1-hour video converts with stable memory.

### Phase 5: polish
- Settings window, first-run tour, dark mode, reduced motion, HEIC/AVIF extras,
  Office-only conversions (7.6), "Start with Windows".

### Phase 6: extras
- Virtual files (5.2), touch/pen dragging, an optional Explorer right-click "Roda"
  entry (portable: per-user `HKCU\Software\Classes\*\shell` key, created and removed from
  Settings), more languages.

## 9. Portability rules and the two builds

- **Settings location:** if `Roda.exe`'s folder is writable, keep `settings.json` and
  `log.txt` in a `data` subfolder next to the exe (portable mode). Otherwise use
  `%APPDATA%\Roda`. Log which one was chosen at startup.
- **Temp files:** `%TEMP%\Roda\<pid>`, deleted on exit and cleaned at next start.
- **No registry writes** except: the optional Explorer menu entry (Phase 6), only on
  request, removed from Settings. Theme detection only *reads* the registry.
- **Start with Windows:** a `.lnk` in `shell:startup` (created/removed from the tray
  menu), so moving the folder only needs the shortcut fixed.
- **Lite build:** `Roda.exe` alone (single file, ~2–5 MB): images, actions, archives,
  documents, spreadsheets, subtitles.
- **Full build:** a portable ZIP: `Roda.exe` + `engines\` (`pdfium.dll`, FFmpeg DLLs,
  libheif DLLs, AVIF encoder) + `licenses\` + `README.txt`. Same Roda.exe; it finds the
  engines in its `engines` folder and enables those options.
- Load engine DLLs with `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
  LOAD_LIBRARY_SEARCH_SYSTEM32)` using a full path, never by bare name (avoids DLL
  hijacking from the current directory).

## 10. Repo layout

```
Roda/
  CLAUDE.md               working rules for agents (short)
  PLAN.md                 this guide
  README.md               user-facing: download, how to use, privacy, verify, build
  THIRD_PARTY.md          every library, version, licence, source URL
  build.sh                zig build: x64 + arm64, Lite exe and Full zip
  .gitattributes
  .github/workflows/build.yml
  res/                    app.rc, app.manifest (per-monitor-v2 DPI, common controls v6), icon
  src/
    common.h util.cpp     (from Ampaian)
    gfx.* anim.*          (from Ampaian)
    app.cpp               tray, loop, single instance, capability detection
    input.cpp             raw input, drag state, trigger keys, probe
    wheel.cpp             wheel window, drop target, drawing, hit-testing, keyboard nav
    cards.cpp             progress and result cards
    jobs.cpp              queue, worker processes, job object, pipes
    worker.cpp            --worker and --convert entry points
    registry.cpp          converter registry, file-kind detection
    conv/
      image.cpp pdf.cpp media.cpp doc.cpp sheet.cpp subtitle.cpp archive.cpp office.cpp
    actions.cpp           copy/move/path/terminal/float/share
    settings_ui.cpp guide.cpp
  third_party/            vendored sources at pinned versions (+ their licences)
  tests/
    corpus/               small sample files of every format, plus broken ones
    run_tests.sh          runs Roda.exe --convert over the corpus, checks outputs
```

## 11. Testing

- **Automated (the agent runs these):** `tests/run_tests.sh` runs every converter through
  `Roda.exe --convert` on the corpus and checks: exit code, output exists, output is
  non-empty, output decodes again (round-trip through WIC/FFmpeg/PDFium where possible),
  dimensions/duration within tolerance. Broken files must exit non-zero with an error
  line, never crash or hang (timeout: 60 s per file).
- **Corpus:** generate most samples yourself (ImageMagick/FFmpeg on the dev machine, or
  with Roda's own encoders once they work). Keep the corpus small (< 30 MB total).
  Include: a CMYK JPEG, a 16-bit PNG, an animated GIF, a transparent PNG, a rotated
  (EXIF) photo, a password PDF, a 300-page PDF, a zero-byte file, a file with a wrong
  extension, a path with spaces and non-Latin characters (`测试 файл.png`), and a path
  longer than 260 characters.
- **By hand (the user runs these):** the trigger matrix (5.3) and the UI. The agent can't
  see the screen reliably: after each phase, give the user a short, numbered checklist of
  what to try, and ask them to send the log lines if something fails.
- **Performance checks:** log idle memory every few seconds in a test mode (like
  Ampaian's `WatchMemory`), and wheel-open latency in ms.

## 12. README content (keep it accurate as features land)

1. One-line description and a short GIF/screenshot (the user makes it).
2. Download: Lite exe and Full zip links (`releases/latest/download/...`), Windows 10/11,
   x64/ARM64, SmartScreen note ("More info → Run anyway").
3. How to use: drag + Shift/Ctrl/Alt, the keyboard hotkey, cancel.
4. **Privacy & permissions:** no network connections; reads only the files you drop;
   writes outputs next to the source; settings and log in `data\` or `%APPDATA%\Roda`;
   temp files in `%TEMP%\Roda`; no registry writes except the optional Explorer entry;
   raw mouse input is used to notice drags (no keyboard or mouse hooks).
5. Verify the download: `gh attestation verify Roda.exe --repo <owner>/roda`, plus the
   SHA-256 on each release.
6. Build from source, licences (`THIRD_PARTY.md`).

## 13. Legal and licensing notes

1. **Look and name:** the interaction (drag + key → wheel) is fine to implement; use your
   own name, icon, colours and artwork. Never use Tangerine's assets or say "Tangerine
   for Windows".
2. **LGPL (FFmpeg, libheif, libde265):** ship them as separate DLLs that users could
   replace, include their licence texts and say where their source is. Don't link them
   statically into a closed-source exe.
3. **OpenH264:** the source is BSD, but Cisco's patent licence only covers Cisco's own
   prebuilt binaries downloaded from Cisco. Prefer Media Foundation's H.264 encoder when
   present; tell the user about this trade-off before shipping OpenH264.
4. **HEVC/HEIC:** decoding HEVC may carry patent obligations in some countries. Fine for a
   free open-source tool; flag it to the user before selling the app.
5. **RAR:** extracting via libarchive is fine. Never create RAR files (RAR compression is
   proprietary).
6. Choose the project licence with the user (MIT recommended) before the first release,
   and add a `LICENSE` file.

## 14. Release and trust

- CI (from Ampaian's workflow) builds both architectures on `windows-latest`, runs the
  CLI tests, packs the Lite exe and Full zip, signs build-provenance attestations
  (`actions/attest-build-provenance`) and publishes a GitHub release on `v*` tags with
  SHA-256 sums in the notes.
- Each release: upload the files to VirusTotal and link the results in the release notes.
- Later: code signing (SignPath Foundation's free programme for open-source projects, or
  Azure Trusted Signing), then optionally the Microsoft Store (MSIX) in addition to the
  portable download.

## 15. Gotchas checklist

- Initialise COM as STA (`OleInitialize`) on the UI thread: OLE drag and drop requires it.
  Workers can use MTA.
- `RegisterDragDrop` needs the window created on that STA thread.
- Layered windows: alpha-0 pixels are transparent to drops (5.2).
- `UpdateLayeredWindow` per frame is fine at wheel size (≈400×400 px); don't animate a
  full-screen layered window.
- Long paths: use `\\?\` prefixed paths in workers, and set `longPathAware` in the
  manifest.
- Unicode everywhere: `W` APIs only; UTF-8 in JSON; test non-Latin file names.
- Files on network drives and OneDrive placeholders (cloud-only) can be slow to open: read
  them in the worker, never on the UI thread.
- Don't block the UI thread for more than a few ms, ever. File system calls, COM calls
  into Office and DLL loads happen in workers.
- Locked files (open in another app): open with `FILE_SHARE_READ | FILE_SHARE_WRITE |
  FILE_SHARE_DELETE`; if that fails, say "the file is in use by another app".
- Elevated sources can't drop on Roda (UIPI). Document it; don't run Roda as admin.

## 16. Working rules for the agent

- Ask the user before: choosing a licence, adding any library not listed here, pushing to
  GitHub, creating releases or tags, or publishing anything.
- Commit after each working step with a short, plain sentence as the message (Ampaian's
  style: "Wheel opens on Shift during a file drag"). **No `Co-Authored-By`, no
  `Claude-Session`, no "Generated with Claude" lines** in commits, PRs or release notes.
- Keep `README.md`, `THIRD_PARTY.md` and the privacy section accurate as features land.
- When something can only be verified on screen, say so plainly and give the user exact
  steps; never claim a UI behaviour works without the user confirming it.
- Prefer deleting scope over shipping something flaky: if a conversion's quality is poor,
  leave it out and tell the user.
