// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scheduling hints for the threads that run the game.
#pragma once

namespace os {

// Asks the OS to treat the calling thread as part of the game's frame loop:
// on macOS, the user-interactive quality of service, so that Apple Silicon
// runs it on a performance core and wakes it without timer coalescing (at
// the default quality of service the game thread could land on an
// efficiency core and miss frames). Elsewhere it does nothing for now.
void set_interactive_thread();

}  // namespace os
