// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP launcher for Mario Kart Arcade GP 2 (see runtime/app/launcher.h for
// the options).

#include "app/launcher.h"
#include "patches.h"
#include "ppc_recomp_shared.h"

int main(int argc, char** argv) {
  app::Game game;
  game.id = "GGPE02";
  game.title = "Mario Kart Arcade GP 2";
  game.module = &guest::g_module;
  game.symbols = OPENGP_SYMBOLS;
  game.set_free_play = mkgp2::set_free_play;
  game.test_menu = mkgp2::test_menu_reboot;
  return app::run(game, argc, argv);
}
