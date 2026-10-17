// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP's installer (see installer.h): builds the games from the player's
// disc images.
//
//   OpenGP [IMAGE...] [--force] [--jobs N] [--cxx COMPILER] [--keep-build]
//
// Without images it uses the images each game was built from before, then
// those in ISO/ and private/ next to it. Each game is built on its own: a
// missing image only skips that game. Games already built by this version
// are skipped unless --force is given. On Windows an image can be dropped on
// OpenGP.exe.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/paths.h"
#include "installer.h"
#include "os/utf8.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr const char* kUsage =
    "usage: OpenGP [IMAGE...] [--force] [--jobs N] [--cxx COMPILER] [--keep-build]\n"
    "\n"
    "Builds the games from your disc images (.iso / .gcm). Without images, it uses\n"
    "the ones the games were built from before, then those in ISO/ and private/.\n";

// When OpenGP.exe was started from Explorer (a double click, or an image
// dropped on it), its console closes when it exits: wait for a key first.
void pause_if_own_console() {
#ifdef _WIN32
  DWORD pids[2];
  if (GetConsoleProcessList(pids, 2) == 1) {
    std::printf("\nPress Enter to close.");
    std::fflush(stdout);
    std::getchar();
  }
#endif
}

int run(const std::vector<std::string>& args) {
  std::vector<std::string> images;
  std::string cxx;
  bool force = false;
  installer::BuildOptions options;
  const std::size_t argc = args.size();
  for (std::size_t i = 1; i < argc; ++i) {
    const std::string& a = args[i];
    if (a == "--force") force = true;
    else if (a == "--keep-build") options.keep_build = true;
    else if (a == "--jobs" && i + 1 < argc) {
      char* end = nullptr;
      const long n = std::strtol(args[++i].c_str(), &end, 10);
      if (*end != '\0' || n < 1 || n > 256) {
        std::fprintf(stderr, "--jobs %s: give a number from 1 to 256\n", args[i].c_str());
        return 2;
      }
      options.jobs = static_cast<unsigned>(n);
    } else if (a == "--cxx" && i + 1 < argc) cxx = args[++i];
    else if (a == "-h" || a == "--help") {
      std::printf("%s", kUsage);
      return 0;
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "%s", kUsage);
      return 2;
    } else {
      images.push_back(a);
    }
  }

  std::string error;
  installer::Package package;
  if (!installer::load_package(app::executable_dir(), &package, &error)) {
    std::fprintf(stderr, "%s\n(OpenGP must be run from its package folder.)\n", error.c_str());
    return 1;
  }
  std::printf("OpenGP %s installer\n\n", package.version.c_str());

  installer::Reporter report;
  report.line = [](const std::string& s) {
    std::printf("\r%s\n", s.c_str());
    std::fflush(stdout);
  };
  report.progress = [](double f) {
    std::printf("\r  %3d%%", static_cast<int>(f * 100 + 0.5));
    std::fflush(stdout);
  };

  const auto found = installer::find_images(package, images, report);
  for (const installer::GameInfo& g : package.games) {
    bool have = false;
    for (const auto& f : found) have |= f.first == &g;
    if (!have) std::printf("%s: no disc image found; skipped.\n", g.title.c_str());
  }
  if (found.empty()) {
    std::printf("\nPut your disc images in the ISO folder next to OpenGP, or drop one on it (see docs/playing.md).\n");
    return 1;
  }

  int failures = 0;
  std::string compiler;
  for (const auto& [game, image] : found) {
    std::printf("\n%s (%s)\n", game->title.c_str(), game->id.c_str());
    if (!force && installer::is_installed(package, *game)) {
      std::printf("Already built by this version (--force rebuilds it).\n");
      continue;
    }
    if (compiler.empty() && !installer::find_compiler(package, cxx, report, &compiler, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    if (!installer::install_game(package, *game, image, compiler, options, report, &error)) {
      std::fprintf(stderr, "\n%s\n", error.c_str());
      ++failures;
    }
  }
  if (failures == 0) std::printf("\nDone. Start a game with its launcher (opengp-mkgp2 or opengp-mkgp1).\n");
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const int code = run(os::utf8_args(argc, argv));
  pause_if_own_console();
  return code;
}
