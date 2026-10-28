# Working rules (short)

- Read `PLAN.md` first; work phase by phase and stop for the user's hand-tests after each phase.
- Stack: C++20, plain Win32 + Direct2D + DirectWrite + WIC, built with Zig (`./build.sh`). No frameworks.
- Never: global hooks (`SetWindowsHookEx`), network code, writing to the registry (except the optional
  Explorer entry, on request), overwriting or deleting a source file, GPL code, packers.
- Style (from Ampaian): short functions, comments that say *why*, plain-English log lines, no exceptions,
  `Com<T>` for COM.
- Commits: one short plain sentence. No `Co-Authored-By`, `Claude-Session` or "Generated with" lines.
- Ask before: choosing a licence, adding a library not in `PLAN.md`, pushing, tagging, releasing.
- Keep `README.md` (especially Privacy) and `THIRD_PARTY.md` accurate as features land.
- UI behaviour is only "working" once the user has confirmed it on screen.
- Trigger self-test: `tests/dragtest.cpp` plays real OLE drags with injected input (moves the pointer).
