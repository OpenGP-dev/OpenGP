// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera_use.h"

#include <atomic>
#include <bit>

#include "ppc/memory.h"

namespace camera_use {

namespace {

std::atomic<bool> g_on{false};
constexpr gekko::u32 kCameraUse = 0x25;

}  // namespace

void set(bool on) { g_on = on; }

void on_settings_loaded(gekko::u8* base, gekko::u32 settings) { base[settings + kCameraUse] = g_on ? 1 : 0; }

void on_power_tick(gekko::u8* base, gekko::u32 board) {
  constexpr float kWait = 0.5f;
  const gekko::u32 state = ppc::load_be<gekko::u32>(base + board + 0x60);
  gekko::u8* seconds = base + board + 0x64;
  if ((state == 2 || state == 3) && std::bit_cast<float>(ppc::load_be<gekko::u32>(seconds)) > kWait)
    ppc::store_be(seconds, std::bit_cast<gekko::u32>(kWait));
}

}  // namespace camera_use
