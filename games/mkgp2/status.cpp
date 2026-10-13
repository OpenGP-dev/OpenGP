// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): what the player is doing, for Discord
// Rich Presence (see common/status.h). The scenes' meanings were found in the
// game's code and by watching them in scripted games.

#include "status.h"

namespace game_status {

namespace {

// The scene the game is in (g_currentSceneState): the game sets it as it
// goes from scene to scene (SetSyncTarget, 8009B558), and linked cabinets
// compare it to stay in step.
constexpr gekko::u32 kScene = 0x806CED74;

constexpr RaceGlobals kRace = {
    .prefix = "gp2",
    .characters = 13,
    .cups = 8,
    .game_mode = 0x806D0EF4,
    .character = 0x806D0EBC,
    .cup = 0x806CED68,  // cup select's order: Yoshi, Mario, Waluigi, DK, ... (clFlowCup_Update)
    .long_layout = 0x806D0EC8,
    .reverse = 0x806D0ED0,
    .round = 0x806D0EF8,
    .engine = 0x806D0F2C,
    .started = 0x806D0EC0,
    .position = 0x806D0FA0,
    .lap = 0x806D0FA4,
    .laps = 0x806D0FA8,
};

}  // namespace

void read(const gekko::u8* base, app::GameStatus* s) {
  using Scene = app::GameStatus::Scene;
  *s = {};
  s->game_image = "gp2";
  const gekko::u32 scene = word(base, kScene);
  // Start-up (the device check) is in the entry scene too, until the game
  // first moves on.
  static bool started_up = false;
  if (scene != 0x02 && scene != 0x36) started_up = true;
  if (!started_up) {
    s->scene = Scene::Starting;
    return;
  }
  bool chosen = false;  // the player has chosen their character
  switch (scene) {
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:  // entry, the attract mode (its demo races too), title
      s->scene = Scene::Attract;
      break;
    case 0x36: s->scene = Scene::Starting; break;  // looking for linked cabinets
    case 0x35: s->scene = Scene::TestMenu; break;
    case 0x08: s->scene = Scene::Menus, s->activity = "Starting a game"; break;
    case 0x09: s->scene = Scene::Menus, s->activity = "Inserting a Mario Card"; break;
    case 0x0B: case 0x0C: s->scene = Scene::Menus, s->activity = "Choosing a class"; break;
    case 0x0D: case 0x14: s->scene = Scene::Menus, s->activity = "Choosing a character"; break;
    case 0x0F: s->scene = Scene::Menus, s->activity = "Choosing a kart", chosen = true; break;
    case 0x13: s->scene = Scene::Menus, s->activity = "Having a photo taken", chosen = true; break;
    case 0x15: s->scene = Scene::Menus, s->activity = "Choosing a cup", chosen = true; break;
    case 0x17: case 0x19: case 0x1A: s->scene = Scene::Menus, s->activity = "Choosing a course", chosen = true; break;
    case 0x1B: s->scene = Scene::Menus, s->activity = "Choosing a round", chosen = true; break;
    case 0x22: s->scene = Scene::Menus, s->activity = "Choosing items", chosen = true; break;
    case 0x25: case 0x26: case 0x2A: case 0x2B: case 0x2C:  // the race's introduction, loading, the race
      s->scene = Scene::Racing, chosen = true;
      break;
    case 0x2D: s->scene = Scene::Finished, chosen = true; break;  // coins, "Continue?"
    default: s->scene = Scene::Menus; break;
  }
  if (chosen) read_race(base, kRace, s);
}

}  // namespace game_status
