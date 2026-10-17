// SPDX-License-Identifier: GPL-3.0-or-later
//
// Names of host functions in this process, for profiles and frame captures:
// from the executable's debug symbols on Windows (dbghelp, which must only be
// used by one thread at a time: these calls take a lock), from the dynamic
// symbol table elsewhere (exported names only). Recompiled game functions
// are named after the game's: guest::__imp__<name>_<address>.
#pragma once

#include <string>

#include "gekko/types.h"

namespace profile {

struct HostSymbol {
  std::string name;     // "" if unknown
  gekko::u64 offset = 0;  // from the function's start (or the module's, if unknown)
  std::string module;   // "" if unknown
};

HostSymbol host_symbol(gekko::u64 address);

// A host function name made readable: "guest::__imp__fn_80012345" becomes
// "fn_80012345", a weak alias "weak.?tolower...default.?__imp__fn_80270FE8..."
// becomes "tolower (fn_80270FE8)".
std::string readable_symbol(const std::string& name);

// Whether a name is a recompiled game function's (before readable_symbol()).
bool is_guest_symbol(const std::string& name);

}  // namespace profile
