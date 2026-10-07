// SPDX-License-Identifier: GPL-3.0-or-later
//
// The games' own debug messages (--game-log). Both games print what they are
// doing through DebugPrintf (about 500 calls each: scene changes, the
// start-up device checks, the camera, the link between cabinets, the card
// reader...), but only when byte 0x2A of their settings is set, which
// start-up clears. The SDK's OSPanic, which reports failed checks, is
// compiled out of both games (OSPanic_Stub, an empty function). With the log
// on, both print to the console.
#pragma once

namespace game_log {

void enable(bool on);

}  // namespace game_log
