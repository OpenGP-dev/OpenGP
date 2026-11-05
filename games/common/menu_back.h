// SPDX-License-Identifier: GPL-3.0-or-later
//
// Going back through the menus. On a cabinet a play can't go back: once the
// class or the character is chosen it stays chosen. OpenGP lets the brake
// pedal (the "Cancel" of the games' own name entry) go back a screen, from
// the character on: the character, the kart, the cup, the course or round,
// the items.
//
// The games' menus are one dispatcher (GP 2's MenuScene_Dispatch) that calls
// the current screen's update each frame. The update returns the next
// screen's number, or -1 to stay; the dispatcher then deletes the screen and
// makes the one with that number, which sets the scene number
// (g_currentSceneState) as it starts. Going back is the update returning the
// previous screen's number.
//
// Not in a linked game: the cabinets' menus keep in step with each other.
#pragma once

#include <functional>
#include <span>

#include "gekko/types.h"
#include "ppc/context.h"

namespace menu_back {

// What the launcher tells the games, while they run (any thread may call).
// `back` is whether going back is wanted now (the brake pressed, the setting
// on, the game not linked); `decide` whether the gas (the games' "Decide") is.
void set(std::function<bool()> back, std::function<bool()> decide);

struct Menus {
  gekko::u32 scene;                    // g_currentSceneState
  std::span<const gekko::u32> starts;  // the screens a play starts from (going back forgets the last play's)
  std::span<const gekko::u32> stops;   // the screens going back returns to, in order
  std::span<const gekko::u32> back;    // the screens that go back
};

// Whether a screen update that returned `next` should go back instead, and
// to where: call it after the update of each screen in `menus`. Returns the
// screen to go to, or `next`.
gekko::u32 after_update(const gekko::u8* base, const Menus& menus, gekko::u32 next);

// A race starts: the menus after it (the next round's, after "Continue?")
// don't go back to this play's.
void forget();

}  // namespace menu_back
