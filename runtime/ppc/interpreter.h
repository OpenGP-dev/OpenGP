// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference interpreter. It is the fallback for guest code that was not
// recompiled and the oracle the differential tests compare recompiled code
// against. It executes through the same semantic helpers (ops.h).
#pragma once

#include "ppc/context.h"

namespace ppc {

struct InterpreterOptions {
  // When the interpreted code calls or tail-calls a function that has a
  // recompiled version, run the recompiled version. Tests turn this off so
  // the interpreter is an independent oracle.
  bool call_native = true;
};

extern InterpreterOptions g_interpreter_options;

// Runs guest code at `entry` as a function call: returns once control
// reaches the LR value held on entry with r1 back at its entry value.
void interpret(Context& ctx, u8* base, u32 entry);

// Runs from `pc` until control reaches `return_address` with r1 == `return_sp`.
void interpret_until(Context& ctx, u8* base, u32 pc, u32 return_address, u32 return_sp);

}  // namespace ppc
