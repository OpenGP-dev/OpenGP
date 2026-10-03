// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2: options implemented by patches.cpp.
#pragma once

#include "gekko/types.h"

namespace mkgp2 {

// Plays cost nothing: no coins needed. Takes effect when the game sets its
// coins up at start-up, or at once if it already has.
void set_free_play(gekko::u8* base, bool on);

}  // namespace mkgp2
