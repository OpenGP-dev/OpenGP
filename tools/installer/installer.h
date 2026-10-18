// SPDX-License-Identifier: GPL-3.0-or-later
//
// The installer builds each game on the player's machine from their own disc
// image: it checks the image's main.dol, recompiles it with gekkorecomp,
// compiles the result with a C++ compiler and links it with the prebuilt
// runtime. Nothing derived from a game is part of the package.
//
// The package (see docs/packaging.md):
//   OpenGP[.exe]            this program
//   sdk/package.toml        version, compiler and linker settings
//   sdk/include/            the runtime's headers
//   sdk/lib/                the runtime, as static libraries
//   sdk/games/<game>/       game.toml, recomp.toml, config/symbols.txt, patches.cpp
//   sdk/games/common/       main.cpp and the code the games share
//   toolchain/              the compiler, downloaded on first use (Windows)
//   opengp-<game>[.exe]     the games, once built
//   user/                   settings and saves
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace installer {

namespace fs = std::filesystem;

struct GameInfo {
  fs::path dir;  // sdk/games/<game>
  std::string id, title, dol_sha1, executable;
};

struct Package {
  fs::path home;  // the package's folder
  fs::path sdk;   // home/sdk
  std::string version;
  // A compiler to download on first use (Windows), or none.
  std::string toolchain_url, toolchain_sha256, toolchain_dir, toolchain_cxx;
  std::vector<std::string> toolchain_exclude;  // archive paths not extracted
  // Compilers to look for on PATH (Linux).
  std::vector<std::string> cxx_candidates;
  std::vector<std::string> cxx_flags, link_flags, libs, system_libs;
  std::string executable_suffix;  // ".exe" on Windows
  // The games' hand-written sources: in sdk/games/common/, and in each game's folder.
  std::vector<std::string> common_sources, game_sources;
  bool link_group = true;         // wrap the libraries in --start-group/--end-group (not Apple's linker)
  std::vector<GameInfo> games;    // the main game (GP 2) first
};

bool load_package(const fs::path& home, Package* package, std::string* error);

// Progress reports: lines of text, and the fraction of the current step done.
struct Reporter {
  std::function<void(const std::string& line)> line;
  std::function<void(double fraction)> progress;
};

// The C++ compiler to build with: `requested` if given, else the package's
// toolchain (downloaded and unpacked first if needed), else one on PATH.
bool find_compiler(const Package& package, const std::string& requested, const Reporter& report, std::string* cxx,
                   std::string* error);

// The game an image holds, if it is one of the package's: by the ID in its
// header, else (a placeholder header) by its main.dol's SHA-1. Use
// install_game, which checks the SHA-1 in any case, to tell whether it is the
// supported release.
const GameInfo* identify_image(const Package& package, const std::string& image);

// The images found for each game: the ones given, else the one each game was
// built from before (user/images.toml), then those in ISO/ and private/.
// Games without an image are left out.
std::vector<std::pair<const GameInfo*, std::string>> find_images(const Package& package,
                                                                   const std::vector<std::string>& given,
                                                                   const Reporter& report);

// Whether the game is already built by this version of the package.
bool is_installed(const Package& package, const GameInfo& game);

struct BuildOptions {
  unsigned jobs = 0;        // 0: one per CPU thread
  bool keep_build = false;  // keep the generated sources and objects (build/<id>/)
  // Set from another thread to stop the build (the compiler runs that have
  // started still finish); install_game then fails with "cancelled".
  const std::atomic<bool>* cancel = nullptr;
};

// Builds the game from `image` into home/<executable>.
bool install_game(const Package& package, const GameInfo& game, const std::string& image, const std::string& cxx,
                  const BuildOptions& options, const Reporter& report, std::string* error);

}  // namespace installer
