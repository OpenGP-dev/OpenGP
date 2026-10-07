// SPDX-License-Identifier: GPL-3.0-or-later
//
// The games' own debug messages; see game_log.h. DebugPrintf and OSPanic_Stub
// are named in both games' symbol files (from the mkgp2-decomp project).

#include "game_log.h"

#include <atomic>
#include <cstdio>
#include <string>

#include "os/printf.h"
#include "ppc_recomp_shared.h"

namespace {

std::atomic<bool> g_enabled{false};

// One "[game]" line per line of the message (some start or end with blank
// lines, which are left out).
void print(const std::string& text) {
  std::string out;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (end > start) out += "[game] " + os::sjis_to_utf8(text.substr(start, end - start)) + "\n";
    start = end + 1;
  }
  std::fputs(out.c_str(), stdout);
  std::fflush(stdout);
}

}  // namespace

namespace game_log {

void enable(bool on) { g_enabled = on; }

}  // namespace game_log

namespace guest {

// DebugPrintf(const char* fmt, ...). Off, the game's own: it prints through
// OSReport when its settings byte allows.
PPC_FUNC(DebugPrintf) {
  if (!g_enabled) return __imp__DebugPrintf(ctx, base);
  print(os::format_guest(ctx, base, ctx.gpr[3], 4));
}

// OSPanic(const char* file, int line, const char* fmt, ...), empty in these
// builds: the game carries on after a failed check.
PPC_FUNC(OSPanic_Stub) {
  if (!g_enabled) return __imp__OSPanic_Stub(ctx, base);
  print("OSPanic " + os::read_guest_string(base, ctx.gpr[3]) + ":" + std::to_string(static_cast<gekko::s32>(ctx.gpr[4])) +
        ": " + os::format_guest(ctx, base, ctx.gpr[5], 6));
}

}  // namespace guest
