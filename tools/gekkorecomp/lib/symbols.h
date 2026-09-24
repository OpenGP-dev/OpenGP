// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reader/writer for decomp-toolkit symbol files (symbols.txt):
//
//   fn_80005678 = .text:0x80005678; // type:function size:0x40 scope:global
//   jumptable_80345678 = .data:0x80345678; // type:object size:0x28 scope:local
//   "name with spaces" = .text:0x80001234; // type:label
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace recomp {

using namespace gekko;

struct Symbol {
  enum class Type { Unknown, Function, Object, Label };

  std::string name;
  std::string section;
  u32 address = 0;
  Type type = Type::Unknown;
  std::optional<u32> size;
  std::string scope;
  std::map<std::string, std::string> attributes;  // everything else (data:, align:, ...)
};

struct SymbolFile {
  std::vector<Symbol> symbols;

  bool load(const std::string& path, std::string* error);
  bool parse(const std::string& text, std::string* error);
  std::string serialize() const;
};

}  // namespace recomp
