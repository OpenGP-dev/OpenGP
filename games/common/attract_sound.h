// SPDX-License-Identifier: GPL-3.0-or-later
//
// The attract mode's sound (the demo that loops before a coin goes in). GP 2
// plays its first pass silently: the object that runs the attract mode is
// made with a "quiet" flag set, the demo scenes it makes copy the flag
// (a quiet scene starts no music or sound), and it clears the flag when a
// pass ends. OpenGP can clear the flag as the object is made, so that every
// pass has sound. The volume is the test menu's ATTRACT setting.
#pragma once

#include "gekko/types.h"

namespace attract_sound {

// Whether every pass has sound (otherwise the first one is quiet, as on a
// cabinet). Takes effect when the attract mode next starts.
void set_every_loop(bool on);

// Call once the attract mode's object is made, with the address of its
// "quiet" flag (a byte).
void on_attract_created(gekko::u8* base, gekko::u32 quiet_flag);

// A game that calls on_attract_created says so from a static initialiser
// (the option is offered only then); hooked() tells.
bool declare_hook();
bool hooked();

}  // namespace attract_sound
