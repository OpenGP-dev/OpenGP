// SPDX-License-Identifier: GPL-3.0-or-later
//
// gekkorecomp configuration (recomp.toml). See docs/architecture.md.
//
//   [input]
//   dol = "../../private/GGPE02/main.dol"   # relative to this file
//   symbols = "config/GGPE02/symbols.txt"
//
//   [output]
//   directory = "generated"
//   file_count = 32              # number of ppc_recomp.N.cpp files (fixed so CMake knows them)
//
//   [analysis]
//   sda_base = 0x80400000        # r13 (_SDA_BASE_), optional
//   sda2_base = 0x80410000       # r2 (_SDA2_BASE_), optional
//   interrupt_checks = true      # poll for interrupts on loop back-edges
//   exception_return = ["OSLoadContext"]  # return after these if ctx.exception_return is set
//
//   [[function]]                 # add or override a function
//   address = 0x80003100
//   size = 0x40
//   name = "optional_name"
//
//   [[jump_table]]               # jump table used by the bctr at `bctr`
//   bctr = 0x80004020
//   table = 0x80350000
//   count = 12
//
//   [[patch]]                    # replace an instruction word
//   address = 0x8003737C
//   value = 0x38600001
//
//   [[hook]]                     # call guest::<function>(ctx, base) before an instruction
//   address = 0x80084850
//   function = "SkipCameraCheck"
//   skip = false                 # true: the hook replaces the instruction
//   return = false               # true: return from the guest function after the hook
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace recomp {

using namespace gekko;

struct FunctionOverride {
  u32 address = 0;
  u32 size = 0;
  std::string name;
};

struct JumpTableOverride {
  u32 bctr = 0;
  u32 table = 0;
  u32 count = 0;
};

struct Hook {
  u32 address = 0;
  std::string function;
  bool skip = false;
  bool return_after = false;
};

struct Config {
  std::string dol_path;
  std::string symbols_path;
  std::string output_directory = "generated";
  u32 file_count = 16;

  std::optional<u32> sda_base;
  std::optional<u32> sda2_base;
  bool interrupt_checks = true;
  // Functions that can end an exception handler (OSLoadContext): after a call
  // to one, the caller returns if the runtime set ctx.exception_return.
  std::set<std::string> exception_return;

  std::vector<FunctionOverride> functions;
  std::vector<JumpTableOverride> jump_tables;
  std::map<u32, u32> patches;  // address -> instruction word
  std::map<u32, Hook> hooks;   // address -> hook

  // Loads a config file. Relative input paths are resolved against the
  // config file's directory.
  bool load(const std::string& path, std::string* error);
};

}  // namespace recomp
