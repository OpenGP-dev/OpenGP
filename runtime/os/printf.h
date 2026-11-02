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

// The games' text is Shift JIS (ASCII below 0x80, then Japanese, such as the
// descriptions of the media board's network errors). Converts it to UTF-8 for
// the console; text that is not Shift JIS, or a host without a converter,
// gets the bytes back unchanged.
std::string sjis_to_utf8(const std::string& text);

// Makes a Windows console show what is printed as UTF-8 (it otherwise uses
// the system's code page); nothing elsewhere.
void use_utf8_console();

// Calls `callback` (on a thread of its own) when the console window is
// closed, which ends the process a few seconds later: time to save. (Windows
// only; a program with windows hears of logging off and shutting down
// through its windows instead.)
void on_console_close(void (*callback)());

}  // namespace os
