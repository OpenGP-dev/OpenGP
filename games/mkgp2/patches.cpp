// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include "patches.h"

#include <array>
#include <atomic>

#include "ppc_recomp_shared.h"

namespace mkgp2 {

namespace {

// The coin settings the game uses (see fn_80289B54): bytes 0xC-0xF are what a
// play costs, per coin slot; at 0 the game is free ("CREDIT(S) 0/0", "Step on
// gas pedal to start").
constexpr gekko::u32 kPlayCosts = 0x80690AA0u + 0xC;

std::atomic<bool> g_free_play{false};
std::array<gekko::u8, 4> g_costs{};           // the operator's settings
std::atomic<bool> g_costs_known{false};

void write_costs(gekko::u8* base, bool free) {
  for (unsigned i = 0; i < 4; ++i) base[kPlayCosts + i] = free ? 0 : g_costs[i];
}

}  // namespace

void set_free_play(gekko::u8* base, bool on) {
  g_free_play = on;
  if (g_costs_known) write_costs(base, on);
}

}  // namespace mkgp2

namespace guest {

// The arcade library's coin set-up: copies the coin settings from the system
// backup (SegaBoot's, normally set in its test menu) to 80690AA0.
PPC_FUNC(fn_80289B54) {
  __imp__fn_80289B54(ctx, base);
  for (unsigned i = 0; i < 4; ++i) mkgp2::g_costs[i] = base[mkgp2::kPlayCosts + i];
  mkgp2::g_costs_known = true;
  if (mkgp2::g_free_play) mkgp2::write_costs(base, true);
}

}  // namespace guest
