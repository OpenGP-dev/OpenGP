// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether to print what is only for working on OpenGP (--verbose): the
// games' SDK messages (OSReport), the hardware's register and unhandled
// accesses, the DSP's microcode, and a status line every 5 seconds. A player's
// console shows only the start-up summary, what happens (a card, a gamepad,
// a restart) and problems.
#pragma once

#include <atomic>

namespace os {

inline std::atomic<bool> g_verbose{false};

inline bool verbose() { return g_verbose.load(std::memory_order_relaxed); }

}  // namespace os
