// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP launcher for Mario Kart Arcade GP (see runtime/app/launcher.h for
// the options).

#include "app/launcher.h"
#include "arcade.h"
#include "ppc_recomp_shared.h"

int main(int argc, char** argv) {
  app::Game game;
  game.id = "GGPE01";
  game.title = "Mario Kart Arcade GP";
  game.module = &guest::g_module;
  game.symbols = OPENGP_SYMBOLS;
  game.set_free_play = arcade::set_free_play;
  game.test_menu = arcade::test_menu_reboot;
  return app::run(game, argc, argv);
}
