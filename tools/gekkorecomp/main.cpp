// SPDX-License-Identifier: GPL-3.0-or-later
//
// gekkorecomp: statically recompiles a GameCube/Triforce DOL into C++.
//
//   gekkorecomp <recomp.toml> [--dol PATH] [--symbols PATH] [--out DIR]

#include <cstdio>
#include <cstring>
#include <string>

#include "analysis.h"
#include "codegen.h"
#include "config.h"
#include "dol.h"
#include "symbols.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: gekkorecomp <recomp.toml> [--dol PATH] [--symbols PATH] [--out DIR]\n"
               "\n"
               "Recompiles the DOL named in the config (or --dol) into C++ sources in the\n"
               "output directory. See docs/architecture.md for the config format.\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path, dol_override, symbols_override, out_override;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](std::string* out) {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (arg == "--dol") {
      if (!value(&dol_override)) return usage(), 2;
    } else if (arg == "--symbols") {
      if (!value(&symbols_override)) return usage(), 2;
    } else if (arg == "--out") {
      if (!value(&out_override)) return usage(), 2;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (config_path.empty()) {
      config_path = arg;
    } else {
      usage();
      return 2;
    }
  }
  if (config_path.empty()) {
    usage();
    return 2;
  }

  std::string error;
  recomp::Config config;
  if (!config.load(config_path, &error)) {
    std::fprintf(stderr, "gekkorecomp: %s\n", error.c_str());
    return 1;
  }
  if (!dol_override.empty()) config.dol_path = dol_override;
  if (!symbols_override.empty()) config.symbols_path = symbols_override;
  if (!out_override.empty()) config.output_directory = out_override;
  if (config.dol_path.empty()) {
    std::fprintf(stderr, "gekkorecomp: no DOL given ([input] dol or --dol)\n");
    return 1;
  }

  recomp::Dol dol;
  if (!dol.load(config.dol_path, &error)) {
    std::fprintf(stderr, "gekkorecomp: %s\n", error.c_str());
    return 1;
  }
  recomp::SymbolFile symbols;
  const bool have_symbols = !config.symbols_path.empty();
  if (have_symbols && !symbols.load(config.symbols_path, &error)) {
    std::fprintf(stderr, "gekkorecomp: %s\n", error.c_str());
    return 1;
  }

  recomp::Program program;
  if (!program.build(dol, have_symbols ? &symbols : nullptr, config, &error)) {
    std::fprintf(stderr, "gekkorecomp: %s\n", error.c_str());
    return 1;
  }
  recomp::GenerateStats stats;
  if (!recomp::generate(program, config.output_directory, &stats, &error)) {
    std::fprintf(stderr, "gekkorecomp: %s\n", error.c_str());
    return 1;
  }
  std::printf("gekkorecomp: %zu functions, %zu instructions, %zu jump tables, %zu warnings -> %s\n",
              stats.functions, stats.instructions, stats.jump_tables,
              stats.warnings + program.warnings().size(), config.output_directory.c_str());
  return 0;
}
