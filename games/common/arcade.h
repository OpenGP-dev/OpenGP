// SPDX-License-Identifier: GPL-3.0-or-later
//
// The arcade library both games are built with (the same code in each, at
// different addresses): the system settings it takes from SegaBoot, and the
// restart into the test menu.
#pragma once

#include "gekko/types.h"
#include "os/reboot.h"

namespace arcade {

// Free play through the coin settings. These are not the games' own but the
// arcade library's (SegaBoot's system settings, from the backup's ACBU0001
// block). Its set-up routine copies them to a block in RAM whose bytes 0xC-0xF
// are what a play costs, per coin slot; at 0 the game is free
// ("CREDIT(S) 0/0").
//
// Call after the game's coin set-up has run, with the settings block's address.
void on_coin_setup(gekko::u8* base, gekko::u32 settings);

// Plays cost nothing: takes effect at the coin set-up, or at once if it has run.
void set_free_play(gekko::u8* base, bool on);

// The restart into the test menu (what the games do when the test button is
// pressed): the library keeps 0x100 bytes at 811FFF00 across the restart, and
// at start-up a first word of 1 means "test mode".
os::Reboot test_menu_reboot();

}  // namespace arcade
