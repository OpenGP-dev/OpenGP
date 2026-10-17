// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native versions of Dolphin SDK functions that need each game's addresses
// (games/common/sdk_hle.cpp has the ones that don't).
#pragma once

#include "ppc/context.h"

namespace sdk_hle {

using GuestFunc = void (*)(ppc::Context& ctx, ppc::u8* base);

// Where the SDK's file system keeps its state: offsets from the small data
// area (r13) of currentDirectory, FstStart, FstStringStart and
// __DVDLongFileNameFlag, and the address of the 256-byte table tolower()
// reads.
struct DvdFs {
  ppc::s32 current_directory, fst_start, fst_string_start, long_file_names;
  ppc::u32 tolower_table;
};

// DVDConvertPathToEntrynum(path), natively: the same lookup as the SDK's
// (relative paths, "." and "..", names compared through the game's
// tolower()), for a call of the recompiled function. `original` handles what
// this doesn't (8.3 name checks, paths outside RAM).
void convert_path_to_entrynum(ppc::Context& ctx, ppc::u8* base, const DvdFs& fs, GuestFunc original);

}  // namespace sdk_hle
