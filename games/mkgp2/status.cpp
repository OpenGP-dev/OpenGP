// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): what the player is doing, for Discord
// Rich Presence (see common/status.h). The scenes' meanings were found in the
// game's code and by watching them in scripted games.

#include "status.h"

#include "camera_use.h"

namespace game_status {

namespace {

// The scene the game is in (g_currentSceneState): the game sets it as it
// goes from scene to scene (SetSyncTarget, 8009B558), and linked cabinets
// compare it to stay in step.
constexpr gekko::u32 kScene = 0x806CED74;

constexpr RaceGlobals kRace = {
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
    .ended = 0x806D0EE9,
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
  bool chosen = false;    // the player has chosen their character
  bool on_track = false;  // the race itself, or its end
  switch (scene) {
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:  // entry, the attract mode (its demo races too), title
      s->scene = Scene::Attract;
      break;
    case 0x36: s->scene = Scene::Starting; break;  // looking for linked cabinets
    case 0x35: s->scene = Scene::TestMenu; break;
    // Getting into a race: the start of a game, the Mario Card, the class, the character and
    // the kart; the photo (its screen says the camera can't be used when there is none); then
    // the cup, the round or course, and the items.
    case 0x08: case 0x09: case 0x0B: case 0x0C: case 0x0D: case 0x0F: case 0x14:
      s->scene = Scene::Menus, s->activity = "Choosing a character";
      break;
    case 0x13:
      s->scene = Scene::Menus, chosen = true;
      s->activity = camera_use::on() ? "Having a photo taken" : "Choosing a character";
      break;
    case 0x15: case 0x17: case 0x19: case 0x1A: case 0x1B: case 0x22:
      s->scene = Scene::Menus, s->activity = "Choosing a course", chosen = true;
      break;
    case 0x25: case 0x26: case 0x2A:  // the race's introduction, loading
      s->scene = Scene::Racing, chosen = true;
      break;
    case 0x2B: case 0x2C: s->scene = Scene::Racing, chosen = on_track = true; break;
    case 0x2D: case 0x34:  // coins, "Continue?"; the hand-over to the results
      s->scene = Scene::Finished, chosen = on_track = true;
      break;
    default: s->scene = Scene::Menus; break;
  }
  if (chosen) read_race(base, kRace, on_track, s);
}

}  // namespace game_status
