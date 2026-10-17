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
    Finished,  // the race is over (`finished`, or the time ran out): results, coins, "Continue?"
    TestMenu,  // the operator's test menu
  };
  Scene scene = Scene::Starting;
  gekko::u32 scene_id = 0;  // the game's own scene number (g_currentSceneState), for profiles
  std::string activity;  // in the menus: "Choosing a character", "Choosing a course"...

  // Image names are art assets' keys in OpenGP's Discord application.
  std::string game_image;  // the game's logo

  // Once chosen (empty until then).
  std::string mode;  // "Grand Prix", "Time Trial", "Versus"
  std::string engine;  // "100cc"
  std::string character, character_image;
  std::string cup, trophy_image;
  std::string course, course_image;  // the name with " (reverse)" for a reversed layout
  int round = 0;                     // a Grand Prix's round, 1-4; 0 otherwise

  // In a race (0 when not known).
  bool started = false;   // the countdown is over
  bool finished = false;  // the player crossed the finish line; `position` is their place
  int position = 0;      // 1 = first
  int lap = 0, laps = 0;
};

}  // namespace app
