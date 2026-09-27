// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP launcher for Mario Kart Arcade GP 2.
//
// Boots the disc image the way the Triforce boot ROM would, installs the
// recompiled game and runs it. The hardware is only partly emulated so far
// (milestone 3); a status line shows how far the game gets.
//
//   opengp-mkgp2 [--iso PATH] [--timeout SECONDS] [--status SECONDS]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#include "disc/disc.h"
#include "hw/vi.h"
#include "machine.h"
#include "ppc/hooks.h"
#include "ppc/trace.h"
#include "ppc_recomp_shared.h"

namespace {

namespace fs = std::filesystem;
using namespace gekko;

constexpr const char* kGameId = "GGPE02";

// Looks for the game's image in ISO/ and private/.
std::string find_image() {
  for (const char* folder : {"ISO", "private"}) {
    const fs::path dir = fs::path(OPENGP_SOURCE_DIR) / folder;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
      const std::string ext = entry.path().extension().string();
      if (ext != ".iso" && ext != ".gcm" && ext != ".ISO" && ext != ".GCM") continue;
      disc::Image img;
      if (img.open(entry.path().string(), nullptr) && img.game_id() == kGameId) return entry.path().string();
    }
  }
  return {};
}

// Function names for backtraces, from the decomp-toolkit symbol file.
std::map<u32, std::string> load_function_names() {
  std::map<u32, std::string> names;
  std::ifstream f(OPENGP_SYMBOLS);
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("type:function") == std::string::npos) continue;
    const auto eq = line.find(" = ");
    const auto colon = line.find(":0x", eq);
    if (eq == std::string::npos || colon == std::string::npos) continue;
    names[static_cast<u32>(std::strtoul(line.c_str() + colon + 3, nullptr, 16))] = line.substr(0, eq);
  }
  return names;
}

std::string describe(const std::map<u32, std::string>& names, u32 addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", addr);
  auto it = names.find(addr);
  return it == names.end() ? std::string(buf) : std::string(buf) + " " + it->second;
}

}  // namespace

int main(int argc, char** argv) {
  std::string image;
  int timeout_s = 20, status_s = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iso" && i + 1 < argc) image = argv[++i];
    else if (a == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
    else if (a == "--status" && i + 1 < argc) status_s = std::atoi(argv[++i]);
    else {
      std::fprintf(stderr, "usage: opengp-mkgp2 [--iso PATH] [--timeout SECONDS] [--status SECONDS]\n");
      return 2;
    }
  }
  if (image.empty()) image = find_image();
  if (image.empty()) {
    std::fprintf(stderr, "No %s image found in ISO/ or private/ (see docs/game-files.md), or pass --iso.\n",
                 kGameId);
    return 1;
  }

  static opengp::Machine machine;
  machine.set_user_dir((fs::path(OPENGP_SOURCE_DIR) / "user").string());
  std::string error;
  os::BootResult boot;
  if (!machine.boot(image, &boot, &error)) {
    std::fprintf(stderr, "boot failed: %s\n", error.c_str());
    return 1;
  }
  machine.install(guest::g_module);
  std::printf("OpenGP: Mario Kart Arcade GP 2 (%s)\n", machine.disc().game_id().c_str());
  std::printf("  image:  %s\n", image.c_str());
  std::printf("  memory: %u MiB, entry %08X, FST %08X, arena %08X-%08X\n", machine.mem1_size() >> 20,
              boot.entry_point, boot.fst_address, boot.arena_lo, boot.arena_hi);
  std::fflush(stdout);

  static const auto names = load_function_names();
  auto print_status = [](const char* why) {
    auto* vi = machine.hardware().device<hw::ViDevice>();
    std::fprintf(stderr, "[opengp] %s: %llu retraces, %llu frames, %llu draws, %d guest threads, PI cause %05X mask %05X\n",
                 why, static_cast<unsigned long long>(vi ? vi->retrace_count() : 0),
                 static_cast<unsigned long long>(machine.gpu().frames()),
                 static_cast<unsigned long long>(machine.gpu().draws()), machine.threads().thread_count(),
                 machine.hardware().interrupt_cause(), machine.hardware().interrupt_mask());
    for (ppc::TraceStack* s : ppc::trace_all_stacks()) {
      const auto frames = ppc::trace_snapshot(*s);
      if (frames.empty()) continue;
      std::fprintf(stderr, "  stack:");
      const std::size_t first = frames.size() > 12 ? frames.size() - 12 : 0;
      for (std::size_t i = frames.size(); i-- > first;) std::fprintf(stderr, "\n    %s", describe(names, frames[i]).c_str());
      std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
  };
  ppc::g_hooks.fatal = [](const char* message) {
    std::fprintf(stderr, "[opengp] fatal: %s\n", message);
  };

  std::thread([timeout_s, status_s, print_status] {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(timeout_s);
    for (;;) {
      auto wake = deadline;
      if (status_s > 0) wake = std::min(wake, clock::now() + std::chrono::seconds(status_s));
      std::this_thread::sleep_until(wake);
      if (clock::now() >= deadline) {
        print_status("timeout");
        std::fflush(stdout);
        std::_Exit(0);
      }
      print_status("status");
    }
  }).detach();

  machine.run();
  print_status("entry point returned");
  return 0;
}
