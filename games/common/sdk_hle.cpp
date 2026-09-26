// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replacements for Dolphin SDK functions, shared by the games. Each one
// overrides the recompiled (weak) function of the same name; see
// runtime/os/threads.h for why the context functions are replaced.
//
// Compiled into each game's executable against that game's generated header.

#include <cstdio>
#include <string>

#include "os/printf.h"
#include "os/threads.h"
#include "ppc_recomp_shared.h"

namespace guest {

// OSReport(const char* fmt, ...): print to the host console.
PPC_FUNC(OSReport) {
  std::string text = os::format_guest(ctx, base, ctx.gpr[3], 4);
  if (text.empty() || text.back() != '\n') text += '\n';
  std::fputs(("[OSReport] " + text).c_str(), stdout);
  std::fflush(stdout);
}

// OSSaveContext runs as recompiled code; the thread manager only needs to
// know which context the calling thread saved.
PPC_FUNC(OSSaveContext) {
  os::g_threads->on_save_context(ctx, ctx.gpr[3]);
  __imp__OSSaveContext(ctx, base);
}

// OSLoadContext switches to another guest thread (or returns from an
// exception); see os::ThreadManager::load_context.
PPC_FUNC(OSLoadContext) { os::g_threads->load_context(ctx, ctx.gpr[3]); }

// Mid-function hook on SelectThread's idle loop (recomp.toml): wait for an
// interrupt instead of spinning a host core.
PPC_HOOK(OSIdleWait) { os::g_threads->idle(ctx); }

// RealMode(func) runs `func` with address translation off. The SDK only uses
// it for Config24MB/Config48MB, which reprogram the BATs for the memory size;
// the memory map is fixed here, so there is nothing to do.
PPC_FUNC(RealMode) {}

}  // namespace guest
