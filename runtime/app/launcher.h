// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher the games share: reads the command line and the player's
// settings, boots the disc image through an emulated Triforce, installs the
// recompiled game and runs it in a window with sound, controls and the menu.
// The game runs on its own thread; the main thread owns the window and
// renders the frames it produces.
//
//   opengp-<game> [--iso PATH] [--user-dir PATH] [--timeout SECONDS] [--status SECONDS]
//                 [--fullscreen | --windowed] [--aspect RATIO] [--scale N]
//                 [--headless] [--no-video] [--screenshot FRAME]... [--screenshot-every N]
//                 [--input SCRIPT] [--input-file PATH] [--free-play] [--test-menu]
//                 [--no-audio] [--volume PERCENT] [--audio-dump PATH] [--game-log]
//                 [--camera off|webcam|test] [--cabinet 1-4] [--lan] [--verbose] [--version]
//
// Options override user/settings.toml for one run (see docs/building.md).
#pragma once

#include <functional>

#include "app/game_status.h"
#include "gekko/types.h"
#include "os/reboot.h"
#include "ppc/module.h"

namespace app {

using namespace gekko;

struct Game {
  const char* id = "";     // disc ID, e.g. "GGPE02"
  const char* title = "";  // e.g. "Mario Kart Arcade GP 2"
  const ppc::RecompiledModule* module = nullptr;
  const char* symbols = nullptr;  // decomp-toolkit symbols.txt, for function names in backtraces
  // SHA-1 of the main.dol the build was recompiled from: the launcher runs only
  // with an image that holds it.
  const char* dol_sha1 = nullptr;
  // Turns free play on or off: at start-up (before the game runs) and from
  // the menu (while it runs). `base` is guest memory.
  std::function<void(u8* base, bool on)> set_free_play;
  // The reboot into the game's test menu (the operator's settings), if the
  // menu offers it.
  std::function<os::Reboot()> test_menu;
  // Prints the game's own debug messages (--game-log), before the game runs.
  std::function<void(bool on)> set_game_log;
  // Whether the game uses its camera (OpenGP plays it), before the game runs.
  std::function<void(bool on)> set_camera;
  // The cabinet's number for linking (1-4, or 0 for the game's own setting),
  // and whether other cabinets have been found that the game is not linked
  // with yet (asked while it starts), before the game runs.
  std::function<void(int number, std::function<bool()> linking)> set_cabinet;
  // What the player is doing, read from guest memory while the game runs
  // (for Discord Rich Presence).
  std::function<void(const u8* base, GameStatus* status)> status;
  // Whether the game has stopped because it lost its link to the other
  // cabinets, read from guest memory while the game runs.
  std::function<bool(const u8* base)> link_error;
  // Going back through the game's menus: `back` says whether the player wants
  // to go back now, `decide` whether they are pressing the games' "Decide".
  std::function<void(std::function<bool()> back, std::function<bool()> decide)> set_menu_back;
};

// Runs the game; returns the process exit code.
int run(const Game& game, int argc, char** argv);

}  // namespace app
