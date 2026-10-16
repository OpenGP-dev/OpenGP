// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which cabinet the game is when cabinets link (milestone 12). Both games keep
// the cabinet's number, its PCB ID (1-4), in their settings block (byte 0x21),
// set in the test menu. Start-up (GP 2's GameBoot_Init) reads it and gives
// the game the address for it from a table of the cabinets' addresses
// (acModifyMyIPaddr: 192.168.29.150 + ID - 1). OpenGP can choose the number
// instead (`[link] cabinet`), so that each OpenGP on a computer or a network
// plays another cabinet.
//
// It also decides how long the game waits for the others as it starts. The
// games look for the other cabinets only then, on their LINK screen: 60
// seconds on a cabinet (BootPCBCheck_Init sets the timer, BootPCBCheck_Poll
// counts it down and ends it early once every cabinet has answered).
// OpenGP skips the wait (recomp.toml), unless other cabinets have been found
// that the game has not linked with yet: then it waits, up to 15 seconds,
// until it has, so that a connection lost on the way is tried again.
#pragma once

#include <functional>

#include "gekko/types.h"

namespace cabinet {

// `number`: 1-4, or 0 to keep the game's own setting. `linking`: whether
// other cabinets have been found that the game is not linked with yet.
void set(int number, std::function<bool()> linking);

// Call after start-up has read the settings, with the settings block's address.
void on_settings_loaded(gekko::u8* base, gekko::u32 settings);

// Call when the game gives itself an address (acModifyMyIPaddr), which
// start-up does before it returns, with the address string's pointer in
// `address`. `table` is the game's list of addresses: for each cabinet, its
// own and its camera's (string pointers). For a chosen number, the settings
// say it and the address is its own.
void on_modify_address(gekko::u8* base, gekko::u32 settings, gekko::u32 table, gekko::u32* address);

// Call after the LINK screen starts, and before each of its frames, with the
// address of its timer (frames left, a word).
void on_link_screen_start(gekko::u8* base, gekko::u32 timer);
void on_link_screen_frame(gekko::u8* base, gekko::u32 timer);

}  // namespace cabinet
