// SPDX-License-Identifier: GPL-3.0-or-later

#include "cabinet.h"

#include <atomic>

#include "ppc/memory.h"

namespace cabinet {

namespace {

std::atomic<int> g_number{0};
constexpr gekko::u32 kPcbId = 0x21;
constexpr int kCabinets = 4;

}  // namespace

void set(int number) { g_number = number >= 1 && number <= kCabinets ? number : 0; }

void on_settings_loaded(gekko::u8* base, gekko::u32 settings) {
  if (const int n = g_number) base[settings + kPcbId] = static_cast<gekko::u8>(n);
}

void on_modify_address(gekko::u8* base, gekko::u32 settings, gekko::u32 table, gekko::u32* address) {
  const int n = g_number;
  if (!n) return;
  for (int i = 0; i < kCabinets; ++i) {
    if (ppc::load_be<gekko::u32>(base + table + 8 * i) != *address) continue;
    *address = ppc::load_be<gekko::u32>(base + table + 8 * (n - 1));
    base[settings + kPcbId] = static_cast<gekko::u8>(n);
    return;
  }
}

}  // namespace cabinet
