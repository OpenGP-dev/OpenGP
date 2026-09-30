// SPDX-License-Identifier: GPL-3.0-or-later
//
// A sampling profiler for development. Every millisecond it records where
// each thread of the process is (its stack), then prints, for each thread
// that was busy, the functions it spent its time in: self time (inlined code
// counts towards the function it was inlined into), then inclusive time.
// Waits show up as system calls (Nt*, Zw*). Names come from the
// executable's debug symbols. Windows only for now.
//
// OPENGP_PROFILE=START,SECONDS profiles SECONDS seconds, beginning START
// seconds after start-up.
#pragma once

namespace os {

// Starts the profiler if OPENGP_PROFILE asks for it.
void start_profiler_from_env();

}  // namespace os
