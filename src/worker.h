// Worker, CLI and info modes of Roda.exe (see worker.cpp).
#pragma once
#include "common.h"

namespace rd {

/// Runs --worker, --convert or --info when argv asks for one; `handled` says whether it did.
int RunOtherMode(int argc, wchar_t** argv, bool& handled);

}  // namespace rd
