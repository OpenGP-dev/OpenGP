// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the player is doing, for Discord Rich Presence: each game's status.cpp
// reads it from the game's globals (the decomp's names), with what both games
// share here: their names for characters, cups and courses, the pictures for
// them (art assets in OpenGP's Discord application, the same for both games),
// and their race globals, which have the same names and meanings in both.
#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "app/game_status.h"
#include "gekko/types.h"
#include "ppc/memory.h"

namespace game_status {

// Reads the status from guest memory (`base`) while the game runs. The game
// may be changing it at the same time: the values are single words, and a
// stale one is shown for a second at most.
void read(const gekko::u8* base, app::GameStatus* status);

struct CharacterName {
  const char* name;
  const char* image;  // the art asset's key
};

// By character ID: the order of the games' character tables (MARIO, LUIGI,
// WARIO, PEACH, KOOPA, KINOPIO, DONKEY, ...). GP has the first 11.
inline constexpr std::array<CharacterName, 13> kCharacterNames = {{
    {"Mario", "mario"},
    {"Luigi", "luigi"},
    {"Wario", "wario"},
    {"Peach", "peach"},
    {"Bowser", "bowser"},
    {"Toad", "toad"},
    {"Donkey Kong", "dk"},
    {"Yoshi", "yoshi"},
    {"Pac-Man", "pacman"},
    {"Ms. Pac-Man", "mspacman"},
    {"Blinky", "blinky"},
    {"Waluigi", "waluigi"},
    {"Mametchi", "mametchi"},
}};

struct CupName {
  const char* name;
  const char* image;
  const char* courses[2];  // its two courses, the short and the long layout
};

// By cup ID, from 1, as the games write them. GP has the first 6.
inline constexpr std::array<CupName, 8> kCupNames = {{
    {"Mario Cup", "cup_mario", {"Mario Highway", "Mario Beach"}},
    {"DK Cup", "cup_dk", {"DK Jungle", "Bananan Ruins"}},
    {"Wario Cup", "cup_wario", {"Diamond City", "Snow Panic"}},
    {"Pac-Man Cup", "cup_pacman", {"Pac Mountain", "Pac Labyrinth"}},
    {"Bowser Cup", "cup_bowser", {"Bowser's Castle", "Castle Wall"}},
    {"Rainbow Cup", "cup_rainbow", {"Rainbow Coaster", "Rainbow Downhill"}},
    {"Yoshi Cup", "cup_yoshi", {"Yoshi Park 1", "Yoshi Park 2"}},
    {"Waluigi Cup", "cup_waluigi", {"Waluigi Stadium", "Arena Stadium"}},
}};

// A game's race globals.
struct RaceGlobals {
  std::size_t characters, cups;  // how many of the names above the game has
  gekko::u32 game_mode;    // g_gameMode: 0 Grand Prix, 1 time trial, 2 versus (linked)
  gekko::u32 character;    // g_characterId
  gekko::u32 cup;          // g_cupId
  gekko::u32 long_layout;  // g_longRoundFlag: the cup's second course
  gekko::u32 reverse;      // g_reverseRoundFlag
  gekko::u32 round;        // g_roundIndex, from 0 (4-7: the cup's second set of rounds)
  gekko::u32 engine;       // g_ccClass: 0-2 for 50, 100, 150cc
  gekko::u32 started;      // g_raceStarted (a byte): the countdown is over
  gekko::u32 position;     // the player's place now, from 0 (GP 2's g_finalRacePosition)
  gekko::u32 lap;          // from 1
  gekko::u32 laps;         // g_totalLaps
};

inline gekko::u32 word(const gekko::u8* base, gekko::u32 address) { return ppc::load_be<gekko::u32>(base + address); }

// Fills in the player's character and, in a race (or just after one), the
// race. `s->scene` is set already.
inline void read_race(const gekko::u8* base, const RaceGlobals& g, app::GameStatus* s) {
  using Scene = app::GameStatus::Scene;
  if (const gekko::u32 c = word(base, g.character); c < g.characters) {
    s->character = kCharacterNames[c].name;
    s->character_image = kCharacterNames[c].image;
  }
  if (s->scene != Scene::Racing && s->scene != Scene::Finished) return;

  const gekko::u32 mode = word(base, g.game_mode);
  s->mode = mode == 0 ? "Grand Prix" : mode == 1 ? "Time Trial" : "Versus";
  if (const gekko::u32 engine = word(base, g.engine); engine < 3)
    s->engine = std::array{"50cc", "100cc", "150cc"}[engine];
  if (const gekko::u32 cup = word(base, g.cup); cup >= 1 && cup <= g.cups) {
    const CupName& c = kCupNames[cup - 1];
    s->cup = c.name;
    s->cup_image = c.image;
    s->course = c.courses[word(base, g.long_layout) ? 1 : 0];
    if (word(base, g.reverse)) s->course += " (reverse)";
  }
  if (mode == 0) s->round = static_cast<int>(word(base, g.round) % 4) + 1;
  s->started = base[g.started] != 0;
  if (mode != 1) s->position = static_cast<int>(word(base, g.position)) + 1;  // alone in a time trial
  s->lap = static_cast<int>(word(base, g.lap));
  s->laps = static_cast<int>(word(base, g.laps));
  if (s->position < 1 || s->position > 8) s->position = 0;
  if (s->lap < 0 || s->lap > 9 || s->laps < 1 || s->laps > 9) s->lap = s->laps = 0;
}

}  // namespace game_status
