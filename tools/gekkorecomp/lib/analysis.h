// SPDX-License-Identifier: GPL-3.0-or-later
//
// Static analysis of guest functions: basic blocks, constant propagation over
// GPRs (to classify memory accesses as RAM / MMIO / gather pipe), and
// jump-table resolution for bctr.
#pragma once

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "dol.h"
#include "gekko/isa.h"
#include "symbols.h"

namespace recomp {

struct Function {
  u32 start = 0;
  u32 end = 0;           // exclusive
  std::string symbol;    // original symbol name (may be empty)
  std::string cpp_name;  // C++ identifier used in generated code
};

// How a load/store's address behaves; mirrors ppc::Mem in the runtime.
enum class AccessKind { Ram, Mmio, GatherPipe, Checked };
const char* access_kind_name(AccessKind k);

struct JumpTable {
  u32 table = 0;
  std::vector<u32> targets;  // in table order (may repeat)
};

struct FunctionAnalysis {
  const Function* function = nullptr;
  std::vector<u32> code;       // instruction words, with config patches applied
  std::vector<gekko::Op> ops;  // decoded
  std::set<u32> labels;        // addresses that are branch targets inside the function
  std::map<u32, AccessKind> accesses;    // memory instruction address -> kind
  std::map<u32, JumpTable> jump_tables;  // bctr address -> table
  std::vector<std::string> warnings;

  u32 word(u32 addr) const { return code[(addr - function->start) / 4]; }
  gekko::Op op(u32 addr) const { return ops[(addr - function->start) / 4]; }
};

class Program {
 public:
  bool build(const Dol& dol, const SymbolFile* symbols, const Config& config, std::string* error);

  const Dol& dol() const { return *dol_; }
  const Config& config() const { return *config_; }
  const std::vector<Function>& functions() const { return functions_; }
  const Function* function_at(u32 entry) const;  // function starting exactly at `entry`
  const std::vector<std::string>& warnings() const { return warnings_; }

  // Instruction word at `addr` with patches applied (0 if not in the DOL).
  u32 instruction(u32 addr) const;

  // Size of the data object starting at `addr`, from the symbol file.
  std::optional<u32> object_size(u32 addr) const;

  FunctionAnalysis analyze(const Function& fn) const;

 private:
  const Dol* dol_ = nullptr;
  const Config* config_ = nullptr;
  std::vector<Function> functions_;
  std::map<u32, std::size_t> by_entry_;
  std::map<u32, u32> object_sizes_;
  std::vector<std::string> warnings_;
};

// Turns an arbitrary symbol name into a valid, non-reserved C++ identifier.
std::string sanitize_identifier(const std::string& name, u32 address);

// Classifies a constant effective address.
AccessKind classify_address(u32 ea);

}  // namespace recomp
