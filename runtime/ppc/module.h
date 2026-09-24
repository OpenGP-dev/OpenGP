// SPDX-License-Identifier: GPL-3.0-or-later
//
// A recompiled executable as produced by gekkorecomp (ppc_func_mapping.cpp),
// and helpers to load the original DOL's contents into guest memory.
#pragma once

#include <cstddef>
#include <string>

#include "ppc/context.h"
#include "ppc/dispatch.h"

namespace ppc {

struct InstructionPatch {
  u32 address;
  u32 value;
};

struct RecompiledModule {
  const FuncMapping* functions;
  std::size_t function_count;
  const InstructionPatch* patches;
  std::size_t patch_count;
  u32 entry_point;
  u32 sda_base;   // r13, 0 if unknown
  u32 sda2_base;  // r2, 0 if unknown
};

// Registers the module's functions in g_functions and applies its instruction
// patches to guest RAM (so the interpreter sees the same code).
void install_module(const RecompiledModule& module, u8* base);

// Copies a DOL's sections into guest memory and clears its BSS. Returns false
// (and fills `error`) on a malformed file or a section outside mapped RAM.
bool load_dol(const u8* data, std::size_t size, u8* base, u32* entry_point, std::string* error);

}  // namespace ppc
