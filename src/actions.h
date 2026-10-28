// Actions that need no conversion: copy or move with Explorer's own engine, copy paths, open a
// terminal, share. And reading what is selected in Explorer, for the keyboard route.
#pragma once
#include "common.h"

namespace rd {

class Cards;

/// Copy or move with Explorer's engine (its progress, conflict questions and undo), on a thread of
/// its own. `folder` empty: ask with a folder picker first. Remembers the folder for the wheel.
void CopyOrMove(const std::vector<wstring>& paths, wstring folder, bool move, Settings& settings, Cards& cards);
void CopyPaths(const std::vector<wstring>& paths, Cards& cards);
void OpenTerminal(const std::vector<wstring>& paths, Cards& cards);
void Share(const std::vector<wstring>& paths, Cards& cards);

/// The items selected in the Explorer window (or desktop) in front; empty if none.
std::vector<wstring> ExplorerSelection();

}  // namespace rd
