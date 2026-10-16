// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether the games use their camera. Both keep it in their settings block
// (byte 0x25, "camera use"), read from the backup at start-up and changed in
// the test menu; with it on, the start-up device check looks for the camera
// and players' photos are taken. OpenGP decides instead: on when it plays the
// camera (--camera), off otherwise, so the device check never waits for a
// camera that is not there.
#pragma once

#include "gekko/types.h"

namespace camera_use {

void set(bool on);
bool on();

// Call after start-up has read the settings, with the settings block's address.
void on_settings_loaded(gekko::u8* base, gekko::u32 settings);

// The camera's power. The wheel's force feedback board also switches it: at
// start-up the game turns it on and waits 20 seconds twice while the camera
// starts (the board's state 1 to 4 at +0x60, the seconds left at +0x64), and
// the device check waits for that. OpenGP's camera starts at once, so the
// waits are cut to half a second.
//
// Call after each tick of the board's state, with the board's address.
void on_power_tick(gekko::u8* base, gekko::u32 board);

}  // namespace camera_use
