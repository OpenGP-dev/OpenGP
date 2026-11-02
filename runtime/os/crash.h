// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash reports: on a fatal host exception (access violation etc.), an abort
// or a fatal error, print what faulted (module and offset, and the guest
// address if the access was into guest memory) and a host backtrace, to the
// console and to a report file, then run a callback (e.g. to print the guest
// call stacks) before the process dies. The first part is written with fixed
// buffers only, so it gets out even when the crash was inside the heap; the
// callback runs afterwards, with a time limit.
//
// Backtrace entries are "module+offset"; resolve them with
//   llvm-symbolizer --obj=<module> <offset>...
#pragma once

#include <cstdio>
#include <string>

#include "gekko/types.h"

namespace os {

// Writes more of the report: called once for the console and once for the
// report file (`out` is stderr, then the file).
using CrashCallback = void (*)(std::FILE* out);

// `guest_base` is the start of the 4 GiB guest address space reservation.
// The report goes to stderr and to `report_path` (UTF-8; "" for none), after
// `header` (what is running: version, game).
void install_crash_handler(const gekko::u8* guest_base, CrashCallback callback, const std::string& report_path = {},
                           const std::string& header = {});

// Lets the crash handler run on this thread even when it has run out of
// stack. Call at the start of each thread that runs guest code.
void prepare_thread_for_crashes();

// Writes a crash report for `reason` (a fatal error or an uncaught exception)
// and ends the process.
[[noreturn]] void crash_now(const char* reason);

// Whether a crash report has begun: the process is ending with exit code 3
// (on Windows, maybe after a message the player has yet to close).
bool crashing();

}  // namespace os
