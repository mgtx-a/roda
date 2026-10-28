# Roda

Roda (Malay for "wheel") is a portable, offline file converter for Windows 10 and 11. Drag a file, hold
Shift, Ctrl or Alt, and a wheel of options opens under the pointer. Drop the file on an option and it is
converted or acted on, locally.

> **Status: Phase 1 prototype.** The trigger and the wheel work; the options are placeholders that only
> write what you chose to the log. Nothing is converted yet.

## Download
Not released yet. Builds will be a single `Roda.exe` (x64 and ARM64): no installer, no admin rights,
no runtime. Run it from any folder, including a USB stick; delete the folder to remove it.

## How to use
1. Start `Roda.exe`. A wheel icon appears in the tray.
2. Start dragging one or more files (from Explorer, the desktop, a browser's downloads, ...).
3. While dragging, press **Shift** (Convert), **Ctrl** (Tools) or **Alt** (Actions). The wheel opens.
   Pressing another of these keys switches wheels.
4. Release the mouse button on an option. To cancel, press **Esc**, drop on the centre of the wheel, or
   release outside the wheel (the files then go wherever you dropped them, as usual).

Keys you already hold when the drag starts (Shift+drag to move, say) don't open the wheel: press the key
once the drag is under way.

The wheel doesn't open while a full screen app (a game, a video, a presentation) is in front.

Known limit: a drag from an app running as administrator can't open the wheel (Windows does not let a
normal app receive drops from an elevated one). Don't run Roda as administrator to get around it.

## Privacy & permissions
Roda makes **no network connections**: no telemetry, no update checks, nothing is uploaded.
What it touches:

- **Mouse:** raw mouse input (button and movement) is used to notice that a drag has started. Roda does
  **not** install keyboard or mouse hooks.
- **Other windows:** when a drag starts, Roda asks Windows which window holds the mouse capture (its class
  and program name go in the log), and whether a full screen app is in front, in which case it stays out
  of the way. It reads nothing else from other apps.
- **Keys:** Shift, Ctrl, Alt and Esc are read only while a file drag is in progress.
- **Files you drop:** only their paths are read for now.
- **Settings and log:** `settings.json` and `log.txt` (kept under 200 KB) in a `data` folder next to
  `Roda.exe`, or in `%APPDATA%\Roda` if that folder is read-only. The log says which.
- **Temporary files:** `%TEMP%\Roda\<process id>`, deleted on exit and cleaned up at the next start.
- **Start with Windows** (tray menu): a `Roda.lnk` shortcut in your Startup folder, removed when you
  untick it.
- **Registry:** nothing is written. The light/dark theme setting is only read.

## Build
Needs Zig 0.17 (C++20, MinGW-style target). The script defaults to the toolchain path on the author's PC;
set `ZIG=zig` to use the one on your `PATH`.

    ./build.sh [outdir] [x64|arm64|all]    # -> out/x64/Roda.exe, out/arm64/Roda.exe

Trigger self-test: `tests/dragtest.cpp` plays real drags with injected input (it moves the pointer);
results of the Phase 1 run are in [tests/phase1-results.md](tests/phase1-results.md).

Licences of third-party code: [THIRD_PARTY.md](THIRD_PARTY.md).
