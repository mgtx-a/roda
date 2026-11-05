// Reading what is selected in Explorer, for the keyboard route.
#pragma once
#include "common.h"

namespace rd {

/// The items selected in the Explorer window (or desktop) in front; empty if none.
std::vector<wstring> ExplorerSelection();
/// An Explorer window or the desktop is the window in front.
bool ExplorerInFront();

}  // namespace rd
