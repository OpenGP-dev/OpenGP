// SPDX-License-Identifier: GPL-3.0-or-later

#include "attract_sound.h"

#include <atomic>

namespace attract_sound {

namespace {

std::atomic<bool> g_every_loop{false};
bool g_hooked = false;  // set during static initialisation, before main()

}  // namespace

void set_every_loop(bool on) { g_every_loop = on; }

bool hooked() { return g_hooked; }

bool declare_hook() { return g_hooked = true; }

void on_attract_created(gekko::u8* base, gekko::u32 quiet_flag) {
  if (g_every_loop) base[quiet_flag] = 0;
}

}  // namespace attract_sound
