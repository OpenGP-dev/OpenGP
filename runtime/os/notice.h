// SPDX-License-Identifier: GPL-3.0-or-later
//
// Messages for the player from any thread (a Mario Card or the game's records
// that could not be saved, say): printed on the console at once, and shown on
// screen by the launcher, which takes them each frame.
#pragma once

#include <string>
#include <vector>

namespace os {

// Thread-safe.
void notify_player(const std::string& message);

// The messages since the last call, oldest first. Thread-safe.
std::vector<std::string> take_player_notices();

}  // namespace os
