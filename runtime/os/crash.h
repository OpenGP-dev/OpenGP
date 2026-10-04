// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crash reports: on a fatal host exception (access violation etc.), print
// what faulted (module and offset, and the guest address if the access was
// into guest memory) and a host backtrace, then run a callback (e.g. to
// print the guest call stacks) before the process dies.
//
// Backtrace entries are "module+offset"; resolve them with
//   llvm-symbolizer --obj=<module> <offset>...
#pragma once

#include "gekko/types.h"

namespace os {

using CrashCallback = void (*)();

// `guest_base` is the start of the 4 GiB guest address space reservation.
void install_crash_handler(const gekko::u8* guest_base, CrashCallback callback);

}  // namespace os
