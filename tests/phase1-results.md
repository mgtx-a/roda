# Phase 1 trigger results

Run on 2026-10-28, Windows 10 Home 22H2 (19045), one 1920×1080 screen at 100%, x64 build (388 KB).
Every drag was a real one: injected mouse and key input (`SendInput`) against real apps or the
`dragtest` helper's own OLE drag source. Roda ran with `RODA_DATA` pointing at a scratch folder;
results come from Roda's log and from whether the helper's window received the drop.

## How the trigger decides (since this run)
1. Button down, then movement past the drag threshold.
2. Full screen app in front on that monitor? Then stop (logged once).
3. Up to 400 ms, on every move and every 16 ms: which window holds the mouse capture in the thread that
   owns the window under the button-down point (or, failing that, the foreground thread)? Each change
   is logged. Only when it is OLE's `CLIPBRDWNDCLASS` is the probe shown.
4. The probe checks for `CF_HDROP`, then gets out of the way; the keys are read only after that.

**Confirmed:** during every OLE drag the capture moved to `CLIPBRDWNDCLASS`, in each source process
(dragtest, explorer, chrome, msedge, wordpad). Selections and window moves never did: the capture stayed
on `Edit`, `RICHEDIT50W`, `_WwG`, `Chrome_WidgetWin_1`, or the window being moved (`GUI_INMOVESIZE`).

## Matrix (PLAN.md 5.3)

| Row | How it was run | Result |
|---|---|---|
| Explorer window, one file | Real Explorer, details view, Shift, drop on WEBP | ✅ Convert wheel, choice logged with the path. Capture `DirectUIHWND` → `CLIPBRDWNDCLASS`; file drag noticed 57–61 ms after the threshold |
| Explorer, 20 mixed files + a folder | Real Explorer, Ctrl+A, Ctrl, drop on the top option | ✅ Actions wheel (forced: mixed types and a folder), all 21 paths logged; noticed 19 ms |
| Desktop icons | Test PNG copied to the desktop, Win+D, Shift | ✅ Convert wheel, choice logged (the test file was then deleted) |
| Chrome downloads | Fresh temporary profile, `chrome://downloads`, the file name | ✅ No key: drop lands on the target. Shift: Convert wheel, choice logged |
| Edge downloads | Fresh temporary profile, `edge://downloads/all`, file name and icon | ⚠️ Not testable this way: Edge itself starts no drag from that page (with no key the drop never arrives either; Roda never saw an OLE capture). Hand test from the downloads flyout |
| Firefox downloads | — | ⏭️ Firefox not installed: hand test |
| Text selection + Shift: Notepad | Press and drag to select, Shift part-way | ✅ No wheel. Capture `Edit` |
| Text selection + Shift: Word | Same, in Word (Office 16) | ✅ No wheel. Capture `_WwG` |
| Text selection + Shift: browser | Same, in Edge on a local page | ✅ No wheel. Capture `Chrome_WidgetWin_1` |
| Dragging selected text + Shift (an OLE text drag) | Ctrl+A, then drag the highlight: WordPad, Edge, Chrome | ✅ No wheel; probe saw no files ("a drag without files"), the text landed on the target |
| Title bar + Shift / + Ctrl | Notepad's title bar, key part-way | ✅ No wheel. Capture `Notepad`, moving or sizing |
| Normal file drag, no key | Real Explorer and the helper | ✅ Drop lands on the target; Roda shows nothing |
| Full screen app in front | Helper window covering the screen, Shift | ✅ "a full screen app is in front (dragtest.exe)"; no wheel, the drop landed normally |
| Esc during the wheel | Helper | ✅ Cancelled, nothing chosen |
| Shift, then Ctrl | Helper | ✅ Switched to Tools, choice logged |
| Explorer running as admin | — | ⏭️ Needs a UAC prompt: hand test |
| 100/150/200%, two monitors | — | ⏭️ One 100% screen here: hand test |
| Windows 11 24H2 | — | ⏭️ Hand test |

## Measurements
- **Key to first frame on screen:** 10.6–12.2 ms once warm. A cold open is 14.2–20.7 ms (one 27.9 ms
  outlier in an earlier run): the 16 ms target is missed on some cold opens. The cost is Direct2D's
  software renderer (WARP) starting up again.
- **Threshold to "file drag noticed":** 19–61 ms for Explorer, 37–43 ms for the helper, 18–23 ms for
  Chrome. With the pointer still, the keys are read every 16 ms; while it moves, at once.
- **CPU while the mouse moves (no drag):** 1672 ms in 60 s at 1000 mouse reports a second (65 000
  injected moves), so **about 2.8% of one core** on a gaming mouse. About 0.35% at a typical 125 Hz.
  Nearly all of it is Windows delivering one `WM_INPUT` per report and waking the thread.
- **Memory:** 2.2–2.3 MB private at idle and throughout the minute of movement. After the first wheel it
  stays at 7–14 MB (WARP's memory), the known open issue.
- **Idle CPU:** 0 (no timers run unless a drag or an animation is in progress).

## Running it again
    zig c++ -O1 -std=c++20 -target x86_64-windows-gnu -municode -fms-extensions tests/dragtest.cpp \
        -o dragtest.exe -lole32 -loleaut32 -lshell32 -luser32 -lgdi32 -luuid
    dragtest file-shift <file>                       # and the other built-in scenarios
    dragtest ext explorer:<folder> <file name> shift
    dragtest ext desktop <file name on the desktop> shift
    dragtest ext "title:text.txt - Notepad" - shift select
    dragtest ext "title:Download history" <file name> shift

The helper moves the real pointer and presses real keys; don't touch the mouse while it runs.
