// SPDX-License-Identifier: GPL-3.0-or-later

#include "arcade.h"

#include <array>
#include <atomic>

namespace arcade {

namespace {

std::atomic<bool> g_free_play{false};
std::atomic<gekko::u32> g_costs_address{0};  // 0 until the set-up has run
std::array<gekko::u8, 4> g_costs{};          // the operator's settings

void write_costs(gekko::u8* base, gekko::u32 address, bool free) {
  for (unsigned i = 0; i < 4; ++i) base[address + i] = free ? 0 : g_costs[i];
}

}  // namespace

void on_coin_setup(gekko::u8* base, gekko::u32 settings) {
  const gekko::u32 costs = settings + 0xC;
  for (unsigned i = 0; i < 4; ++i) g_costs[i] = base[costs + i];
  g_costs_address = costs;
  if (g_free_play) write_costs(base, costs, true);
}

void set_free_play(gekko::u8* base, bool on) {
  g_free_play = on;
  if (const gekko::u32 costs = g_costs_address) write_costs(base, costs, on);
}

os::Reboot test_menu_reboot() {
  os::Reboot r;
  r.restart = true;
  r.region_start = 0x811FFF00;
  r.region_end = 0x81200000;
  r.region.assign(0x100, 0);
  r.region[3] = 1;
  return r;
}

}  // namespace arcade
