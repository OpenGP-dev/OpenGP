// SPDX-License-Identifier: GPL-3.0-or-later

#include "menu_back.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "ppc/memory.h"

namespace menu_back {

namespace {

std::mutex g_mutex;
std::function<bool()> g_back, g_decide;

// Only the game's thread runs the menus.
struct State {
  gekko::u32 screen = ~0u;           // the screen of the last update
  std::vector<gekko::u32> stops;     // this play's screens that going back returns to
  bool armed = false;                // the brake was up since the screen started
  bool decided = false;              // the gas was pressed on this screen
} g_state;

constexpr gekko::u32 kStay = ~0u;  // an update's -1

bool has(std::span<const gekko::u32> list, gekko::u32 v) { return std::find(list.begin(), list.end(), v) != list.end(); }

}  // namespace

void set(std::function<bool()> back, std::function<bool()> decide) {
  std::lock_guard lock(g_mutex);
  g_back = std::move(back);
  g_decide = std::move(decide);
}

void forget() {
  g_state.stops.clear();
  g_state.screen = ~0u;
}

gekko::u32 after_update(const gekko::u8* base, const Menus& menus, gekko::u32 next) {
  State& s = g_state;
  const gekko::u32 screen = ppc::load_be<gekko::u32>(base + menus.scene);
  if (screen != s.screen) {
    s.screen = screen;
    s.armed = s.decided = false;
    if (has(menus.starts, screen)) s.stops.clear();
    if (has(menus.stops, screen)) {
      // Back to a screen already in this play (going back): forget the ones after it.
      if (auto it = std::find(s.stops.begin(), s.stops.end(), screen); it != s.stops.end())
        s.stops.erase(it + 1, s.stops.end());
      else
        s.stops.push_back(screen);
    }
  }
  if (!has(menus.back, screen)) return next;

  bool back = false, decide = false;
  {
    std::lock_guard lock(g_mutex);
    back = g_back && g_back();
    decide = g_decide && g_decide();
  }
  // A screen that has been decided is on its way to the next one.
  if (decide) s.decided = true;
  // A brake held from the last screen doesn't go back again.
  if (!back) s.armed = true;
  if (next != kStay || s.decided || !s.armed || !back) return next;

  // The screen before this one (this one is the last stop, unless it isn't
  // a stop itself).
  const std::size_t last = s.stops.size() - (!s.stops.empty() && s.stops.back() == screen ? 1 : 0);
  if (last == 0) return next;
  s.armed = false;
  return s.stops[last - 1];
}

}  // namespace menu_back
