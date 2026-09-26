// SPDX-License-Identifier: GPL-3.0-or-later
//
// Emulates what the GameCube/Triforce boot ROM (IPL, "BS2") does before a
// game starts: it writes the low-memory boot globals and runs the disc's
// apploader, which loads the executable and the file system table (FST) and
// sets up the memory map pointers the SDK reads in OSInit.
//
// Based on Dolphin's Boot_BS2Emu.cpp (CBoot::EmulatedBS2_GC, SetupGCMemory,
// RunApploader), Copyright Dolphin Emulator Project, GPL-2.0-or-later.
#pragma once

#include <string>

#include "disc/disc.h"
#include "ppc/context.h"

namespace os {

using namespace gekko;

struct BootResult {
  u32 entry_point = 0;  // returned by the apploader
  u32 fst_address = 0;  // 0x80000038
  u32 arena_lo = 0;     // 0x80000030
  u32 arena_hi = 0;     // 0x80000034
  u32 bi2_address = 0;  // 0x800000F4
};

// Boots `disc` into guest memory. `ctx` is left in the state the IPL hands
// to the game (MSR, HIDs, stack). The apploader runs in the interpreter.
bool boot_disc(const disc::Image& disc, u8* base, u32 mem1_size, ppc::Context& ctx, BootResult* result,
               std::string* error);

}  // namespace os
