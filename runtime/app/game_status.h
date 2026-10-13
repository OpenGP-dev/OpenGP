// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the player is doing, read from the game's state (its globals, by the
// decomp's names) for Discord Rich Presence. Each game reads its own; the
// launcher turns it into what Discord shows (app/rich_presence.h).
#pragma once

#include <string>

#include "gekko/types.h"

namespace app {

struct GameStatus {
  enum class Scene : gekko::u8 {
    Starting,  // booting, checking its devices
    Attract,   // the attract mode, waiting for a player
    Menus,     // a player is setting up a game (`activity` says what, if known)
    Racing,    // in a race: its countdown, the race itself
    Finished,  // the race is over: results, coins, "Continue?"
    TestMenu,  // the operator's test menu
  };
  Scene scene = Scene::Starting;
  std::string activity;  // in the menus: "Choosing a character", "Choosing a course"...

  // Image names are art assets' keys in OpenGP's Discord application.
  std::string game_image;  // the game's logo

  // Once chosen (empty until then).
  std::string mode;  // "Grand Prix", "Time Trial", "Versus"
  std::string engine;  // "100cc"
  std::string character, character_image;
  std::string cup, cup_image;
  std::string course;  // with " (reverse)" for a reversed layout
  int round = 0;       // a Grand Prix's round, 1-4; 0 otherwise

  // In a race (0 when not known).
  bool started = false;  // the countdown is over
  int position = 0;      // 1 = first
  int lap = 0, laps = 0;
};

}  // namespace app
