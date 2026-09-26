// SPDX-License-Identifier: GPL-3.0-or-later
//
// printf-style formatting of guest variadic calls (OSReport, OSPanic, ...).
#pragma once

#include <string>

#include "ppc/context.h"

namespace os {

using namespace gekko;

// Reads a NUL-terminated guest string (bounded).
std::string read_guest_string(const u8* base, u32 addr, std::size_t max = 4096);

// Formats the guest format string at `fmt_addr` with arguments taken as the
// PowerPC EABI passes them to a variadic function: integers from
// r<first_gpr>..r10 then the caller's stack, doubles from f1..f8.
std::string format_guest(const ppc::Context& ctx, const u8* base, u32 fmt_addr, u32 first_gpr);

}  // namespace os
