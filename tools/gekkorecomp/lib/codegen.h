// SPDX-License-Identifier: GPL-3.0-or-later
//
// C++ code generation for analysed guest functions.
#pragma once

#include <string>

#include "analysis.h"

namespace recomp {

struct GenerateStats {
  std::size_t functions = 0;
  std::size_t instructions = 0;
  std::size_t jump_tables = 0;
  std::size_t checked_accesses = 0;
  std::size_t io_accesses = 0;
  std::size_t warnings = 0;
};

// Writes into `directory`:
//   ppc_recomp_shared.h      declarations of every function and hook
//   ppc_recomp.<N>.cpp       function bodies (config.file_count files)
//   ppc_func_mapping.cpp     address -> function table, patches, module info
//   recomp_report.txt        statistics and warnings
bool generate(const Program& program, const std::string& directory, GenerateStats* stats,
              std::string* error);

// Generates the body of one function (exposed for tests).
std::string emit_function(const Program& program, const FunctionAnalysis& analysis);

}  // namespace recomp
