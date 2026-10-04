// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replacements for Dolphin SDK functions, shared by the games. Each one
// overrides the recompiled (weak) function of the same name; see
// runtime/os/threads.h for why the context functions are replaced.
//
// Compiled into each game's executable against that game's generated header.

#include <algorithm>
#include <cstdio>
#include <string>

#include "os/printf.h"
#include "os/reboot.h"
#include "os/threads.h"
#include "ppc_recomp_shared.h"

namespace {

// The memory the game asked to keep across a restart (OSSetSaveRegion).
gekko::u32 g_save_start = 0, g_save_end = 0;

void write_be32(gekko::u8* base, gekko::u32 address, gekko::u32 value) {
  for (int i = 0; i < 4; ++i) base[address + i] = static_cast<gekko::u8>(value >> (24 - 8 * i));
}

}  // namespace

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

// OSSetSaveRegion(start, end): memory to keep across a restart.
PPC_FUNC(OSSetSaveRegion) {
  g_save_start = ctx.gpr[3];
  g_save_end = ctx.gpr[4];
  __imp__OSSetSaveRegion(ctx, base);
}

// OSResetSystem(reset, resetCode, forceMenu): the launcher restarts the
// process (see os/reboot.h), keeping the save region for a restart
// (OS_RESET_RESTART); hot resets and shutdowns boot afresh.
PPC_FUNC(OSResetSystem) {
  os::Reboot r;
  r.restart = ctx.gpr[3] == 0;
  r.reset_code = ctx.gpr[4];
  constexpr gekko::u32 kRam = 0x80000000, kMaxRegion = 0x01000000;
  const gekko::u32 start = g_save_start | kRam, end = g_save_end | kRam;
  if (r.restart && g_save_end && start < end && end - start <= kMaxRegion) {
    r.region_start = start;
    r.region_end = end;
    r.region.assign(base + start, base + end);
  }
  os::reboot(r);
}

// __OSGetExecParams(params): OSInit asks how it was booted. After a restart,
// the reset code and the memory kept come from the previous run (os/reboot.h);
// the memory is put back now, before OSInit clears the rest of the arena.
// (symbols.txt names it without the leading underscores, which the
// recompiler would otherwise have to rename.)
PPC_FUNC(OSGetExecParams) {
  const os::Reboot* r = os::g_previous_boot;
  if (!r || !r->restart) {
    __imp__OSGetExecParams(ctx, base);
    return;
  }
  std::fprintf(stderr, "[os] booting after a restart (reset code %08X, %zu bytes kept)\n", r->reset_code,
               r->region.size());
  // OSExecParams: valid, restartCode, bootDol, regionStart, regionEnd, argsUseDefault, argsAddr.
  const gekko::u32 p = ctx.gpr[3];
  const gekko::u32 fields[7] = {1, r->reset_code, 0, r->region_start, r->region_end, 1, 0};
  for (int i = 0; i < 7; ++i) write_be32(base, p + 4 * i, fields[i]);
  if (!r->region.empty()) std::copy(r->region.begin(), r->region.end(), base + r->region_start);
}

// RealMode(func) runs `func` with address translation off. The SDK only uses
// it for Config24MB/Config48MB, which reprogram the BATs for the memory size;
// the memory map is fixed here, so there is nothing to do.
PPC_FUNC(RealMode) {}

}  // namespace guest
