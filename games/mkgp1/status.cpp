// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP (GGPE01): what the player is doing, for Discord Rich
// Presence (see common/status.h). GP numbers its scenes differently from GP 2;
// their meanings were found by watching them in scripted games, and the race
// globals sit as in GP 2.

#include "status.h"

namespace game_status {

namespace {

// g_currentSceneState, set by fn_801F0F14 (GP 2's SetSyncTarget).
constexpr gekko::u32 kScene = 0x805B7C3C;

constexpr RaceGlobals kRace = {
    .prefix = "gp1",
    .characters = 11,
    .cups = 6,
    .game_mode = 0x805B8AC0,
    .character = 0x805B8A84,
    .cup = 0x805B7C30,
    .long_layout = 0x805B8A90,
    .reverse = 0x805B8A98,
    .round = 0x805B8AC4,
    .engine = 0x805B8AF0,
    .started = 0x805B8A88,
    .position = 0x805B8B50,
    .lap = 0x805B8B54,
    .laps = 0x805B8B58,
};

}  // namespace

void read(const gekko::u8* base, app::GameStatus* s) {
  using Scene = app::GameStatus::Scene;
  *s = {};
  s->game_image = "gp1";
  const gekko::u32 scene = word(base, kScene);
  // Start-up (the device check) is in the entry scene too, until the game
  // first moves on.
  static bool started_up = false;
  if (scene != 0x02 && scene != 0x21) started_up = true;
  if (!started_up) {
    s->scene = Scene::Starting;
    return;
  }
  bool chosen = false;  // the player has a character
  switch (scene) {
    case 0x02: case 0x03: case 0x04:  // entry, the attract mode (its demo races too)
      s->scene = Scene::Attract;
      break;
    case 0x21: s->scene = Scene::Starting; break;  // looking for linked cabinets (BootPCBCheck_Init)
    case 0x20: s->scene = Scene::TestMenu; break;
    case 0x08: case 0x0A: case 0x0B: s->scene = Scene::Menus, s->activity = "Inserting a Mario Card"; break;
    case 0x0E: s->scene = Scene::Menus, s->activity = "Having a photo taken", chosen = true; break;
    case 0x0F: s->scene = Scene::Menus, s->activity = "Choosing a race", chosen = true; break;  // mode, cup, round
    case 0x18: case 0x1A: case 0x1B: case 0x1C:  // the race's introduction, loading, the race
      s->scene = Scene::Racing, chosen = true;
      break;
    case 0x1D: s->scene = Scene::Finished, chosen = true; break;
    default: s->scene = Scene::Menus; break;
  }
  if (chosen) read_race(base, kRace, s);
}

}  // namespace game_status
