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
//                 [--input SCRIPT] [--input-file PATH] [--free-play]
//                 [--no-audio] [--volume PERCENT] [--audio-dump PATH]
//
// Options override user/settings.toml for one run (see docs/building.md).
#pragma once

#include <functional>

#include "gekko/types.h"
#include "ppc/module.h"

namespace app {

using namespace gekko;

struct Game {
  const char* id = "";     // disc ID, e.g. "GGPE02"
  const char* title = "";  // e.g. "Mario Kart Arcade GP 2"
  const ppc::RecompiledModule* module = nullptr;
  const char* symbols = nullptr;  // decomp-toolkit symbols.txt, for function names in backtraces
  // Turns free play on or off: at start-up (before the game runs) and from
  // the menu (while it runs). `base` is guest memory.
  std::function<void(u8* base, bool on)> set_free_play;
};

// Runs the game; returns the process exit code.
int run(const Game& game, int argc, char** argv);

}  // namespace app
