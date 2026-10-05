// SPDX-License-Identifier: GPL-3.0-or-later
//
// The games' launcher (see runtime/app/launcher.h for the options). The build
// or the installer compiles it for each game, with the game's manifest
// (game.toml) as definitions:
//   OPENGP_GAME_ID, OPENGP_GAME_TITLE, OPENGP_DOL_SHA1, OPENGP_SYMBOLS.

#include "app/launcher.h"
#include "arcade.h"
#include "ppc_recomp_shared.h"

int main(int argc, char** argv) {
  app::Game game;
  game.id = OPENGP_GAME_ID;
  game.title = OPENGP_GAME_TITLE;
  game.module = &guest::g_module;
  game.symbols = OPENGP_SYMBOLS;
  game.dol_sha1 = OPENGP_DOL_SHA1;
  // Both games are built on the same arcade library.
  game.set_free_play = arcade::set_free_play;
  game.test_menu = arcade::test_menu_reboot;
  return app::run(game, argc, argv);
}
