// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scripted input: parsing and timing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "input/menu_chord.h"
#include "input/script.h"

using input::Script;
using triforce::Controls;

namespace {

Controls at(const Script& s, gekko::u64 frame) {
  Controls c;
  s.apply(frame, c);
  return c;
}

}  // namespace

TEST_CASE("buttons are held for the default duration") {
  Script s;
  std::string error;
  REQUIRE(s.parse("600 coin", &error));
  CHECK_FALSE(at(s, 599).coin);
  CHECK(at(s, 600).coin);
  CHECK(at(s, 600 + Script::kDefaultDuration - 1).coin);
  CHECK_FALSE(at(s, 600 + Script::kDefaultDuration).coin);
  CHECK(s.end() == 600 + Script::kDefaultDuration);
}

TEST_CASE("ranges, relative times and analog values") {
  Script s;
  std::string error;
  REQUIRE(s.parse("100 start; +50 item\n+0-20 gas=255 steer=0x10, brake=7  # comment\n1000-2000 steer=200", &error));
  CHECK(at(s, 100).start);
  CHECK(at(s, 150).item);
  CHECK(at(s, 150).gas == 255);
  CHECK(at(s, 169).steering == 0x10);
  CHECK(at(s, 169).brake == 7);
  CHECK(at(s, 170).gas == 0);
  CHECK(at(s, 170).steering == 0x80);
  CHECK(at(s, 1999).steering == 200);
  CHECK(at(s, 2000).steering == 0x80);
}

TEST_CASE("later events win for analog values; buttons combine") {
  Script s;
  std::string error;
  REQUIRE(s.parse("0-100 gas=10 item; 50-60 gas=20 start", &error));
  CHECK(at(s, 55).gas == 20);
  CHECK(at(s, 55).item);
  CHECK(at(s, 55).start);
  CHECK(at(s, 70).gas == 10);
}

TEST_CASE("the player's controls show through") {
  Script s;
  std::string error;
  REQUIRE(s.parse("10 coin", &error));
  Controls c;
  c.gas = 99;
  c.item = true;
  s.apply(10, c);
  CHECK(c.coin);
  CHECK(c.item);
  CHECK(c.gas == 99);
}

TEST_CASE("errors leave the script unchanged") {
  Script s;
  std::string error;
  REQUIRE(s.parse("10 coin", &error));
  CHECK_FALSE(s.parse("20 start; 30 jump", &error));
  CHECK(error.find("jump") != std::string::npos);
  CHECK_FALSE(s.parse("x coin", &error));
  CHECK_FALSE(s.parse("40", &error));
  CHECK_FALSE(s.parse("50-40 coin", &error));
  CHECK_FALSE(s.parse("50 gas=256", &error));
  CHECK_FALSE(at(s, 20).start);
  CHECK(s.end() == 10 + Script::kDefaultDuration);
}

// ---- Back + Start opens the menu ----

namespace {

using input::MenuChord;
using namespace std::chrono_literals;

struct ChordRun {
  MenuChord chord;
  MenuChord::Clock::time_point now{};
  Controls poll(bool back, bool start, bool coin, bool start_control) {
    Controls c;
    c.coin = coin;
    c.start = start_control;
    chord.filter(back != start, back && start, now, &c);
    now += 16ms;
    return c;
  }
};

}  // namespace

TEST_CASE("Back then Start opens the menu without a coin") {
  ChordRun r;
  CHECK_FALSE(r.poll(true, false, true, false).coin);  // Back: the coin is held back
  CHECK_FALSE(r.poll(true, false, true, false).coin);
  const Controls both = r.poll(true, true, true, true);  // the chord
  CHECK_FALSE(both.coin);
  CHECK_FALSE(both.start);
  CHECK_FALSE(r.poll(false, true, false, true).start);  // still nothing until all released
  CHECK_FALSE(r.poll(false, false, false, false).coin);
  CHECK_FALSE(r.poll(false, false, false, false).coin);  // no replay of the held-back coin
}

TEST_CASE("a quick tap of Back still inserts a coin") {
  ChordRun r;
  CHECK_FALSE(r.poll(true, false, true, false).coin);
  CHECK(r.poll(false, false, false, false).coin);  // replayed
  CHECK(r.poll(false, false, false, false).coin);
  CHECK(r.poll(false, false, false, false).coin);
  CHECK_FALSE(r.poll(false, false, false, false).coin);
}

TEST_CASE("holding Back alone reaches the game after the wait") {
  ChordRun r;
  bool seen = false;
  for (int i = 0; i < 12; ++i) seen |= r.poll(true, false, true, false).coin;
  CHECK(seen);
  CHECK(r.poll(true, false, true, false).coin);  // and stays down while held
}

TEST_CASE("other controls are never held back") {
  ChordRun r;
  Controls c;
  c.item = true;
  c.gas = 200;
  r.chord.filter(true, false, r.now, &c);
  CHECK(c.item);
  CHECK(c.gas == 200);
}
