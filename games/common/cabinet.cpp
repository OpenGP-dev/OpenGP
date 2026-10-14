// SPDX-License-Identifier: GPL-3.0-or-later

#include "cabinet.h"

#include <atomic>
#include <utility>

#include "ppc/memory.h"

namespace cabinet {

namespace {

std::atomic<int> g_number{0};
std::function<bool()> g_linking;  // set before the game runs
constexpr gekko::u32 kPcbId = 0x21;
constexpr int kCabinets = 4;
constexpr gekko::u32 kLinkWait = 15 * 60;  // frames

bool linking() { return g_linking && g_linking(); }

}  // namespace

void set(int number, std::function<bool()> linking) {
  g_number = number >= 1 && number <= kCabinets ? number : 0;
  g_linking = std::move(linking);
}

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

void on_link_screen_start(gekko::u8* base, gekko::u32 timer) {
  if (linking()) ppc::store_be<gekko::u32>(base + timer, kLinkWait);
}

void on_link_screen_frame(gekko::u8* base, gekko::u32 timer) {
  if (static_cast<gekko::s32>(ppc::load_be<gekko::u32>(base + timer)) > 0 && !linking())
    ppc::store_be<gekko::u32>(base + timer, 0);
}

}  // namespace cabinet
