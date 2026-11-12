// SPDX-License-Identifier: GPL-3.0-or-later
// Local staging validation: synthetic scene memory, no game data.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "menu_back.h"
#include "ppc/memory.h"

namespace {
constexpr gekko::u32 stay = ~0u;
constexpr gekko::u32 starts[] = {0x09};
constexpr gekko::u32 stops[] = {0x0B, 0x0D, 0x0F, 0x15, 0x17, 0x19, 0x1B, 0x21, 0x22};
constexpr gekko::u32 back_screens[] = {0x0D, 0x0F, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x21, 0x22};
constexpr menu_back::Menus menus{0, starts, stops, back_screens};
struct Play {
  gekko::u8 memory[4]{};
  bool brake = false, gas = false;
  Play() {
    menu_back::forget();
    menu_back::set([this] { return brake; }, [this] { return gas; });
  }
  ~Play() { menu_back::set({}, {}); }
  gekko::u32 frame(gekko::u32 screen, gekko::u32 next = stay) {
    ppc::store_be<gekko::u32>(memory, screen);
    return menu_back::after_update(memory, menus, next);
  }
};
}

TEST_CASE("fresh brake returns character to class, but class cannot go back") {
  Play p;
  CHECK(p.frame(0x0B) == stay);
  CHECK(p.frame(0x0D) == stay);
  p.brake = true;
  CHECK(p.frame(0x0D) == 0x0B);
  CHECK(p.frame(0x0B) == stay);
}

TEST_CASE("held brake cannot back through multiple screens") {
  Play p;
  p.frame(0x0B); p.frame(0x0D); p.frame(0x15);
  p.brake = true;
  CHECK(p.frame(0x15) == 0x0D);
  CHECK(p.frame(0x0D) == stay);
  p.brake = false;
  CHECK(p.frame(0x0D) == stay);
  p.brake = true;
  CHECK(p.frame(0x0D) == 0x0B);
}

TEST_CASE("photo is skipped as a return destination") {
  Play p;
  p.frame(0x0B); p.frame(0x0D); p.frame(0x13); p.frame(0x15);
  p.brake = true;
  CHECK(p.frame(0x15) == 0x0D);
}

TEST_CASE("card kart and round history is retained in order") {
  Play p;
  p.frame(0x0B); p.frame(0x0D); p.frame(0x0F); p.frame(0x13); p.frame(0x15);
  p.brake = true;
  CHECK(p.frame(0x15) == 0x0F);
  p.brake = false;
  p.frame(0x0F); p.frame(0x15); p.frame(0x1B); p.frame(0x21);
  p.brake = true;
  CHECK(p.frame(0x21) == 0x1B);
}

TEST_CASE("gas latches confirmation until a different screen") {
  Play p;
  p.frame(0x0B); p.frame(0x0D);
  p.gas = true;
  CHECK(p.frame(0x0D) == stay);
  p.gas = false; p.brake = true;
  CHECK(p.frame(0x0D) == stay);
  p.brake = false;
  p.frame(0x15);
  p.brake = true;
  CHECK(p.frame(0x15) == 0x0D);
}

TEST_CASE("simultaneous gas and brake confirms rather than returning") {
  Play p;
  p.frame(0x0B); p.frame(0x0D);
  p.brake = p.gas = true;
  CHECK(p.frame(0x0D) == stay);
}

TEST_CASE("normal forward transitions take priority") {
  Play p;
  p.frame(0x0B); p.frame(0x0D);
  p.brake = true;
  CHECK(p.frame(0x0D, 0x13) == 0x13);
}

TEST_CASE("race start and new play discard earlier history") {
  Play p;
  p.frame(0x0B); p.frame(0x0D); p.frame(0x15); p.frame(0x1B);
  menu_back::forget();
  p.frame(0x1B);
  p.brake = true;
  CHECK(p.frame(0x1B) == stay);
  p.brake = false;
  p.frame(0x09); p.frame(0x0D);
  p.brake = true;
  CHECK(p.frame(0x0D) == stay);
}

TEST_CASE("absent callbacks cannot trigger back") {
  Play p;
  p.frame(0x0B); p.frame(0x0D);
  menu_back::set({}, {});
  p.brake = true;
  CHECK(p.frame(0x0D) == stay);
}
