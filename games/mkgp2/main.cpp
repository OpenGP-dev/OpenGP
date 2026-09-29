// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP launcher for Mario Kart Arcade GP 2.
//
// Boots the disc image the way the Triforce boot ROM would, installs the
// recompiled game and runs it in a window. The game runs on its own thread;
// the main thread owns the window and renders the frames it produces.
//
//   opengp-mkgp2 [--iso PATH] [--timeout SECONDS] [--status SECONDS] [--scale N]
//                [--headless] [--no-video] [--screenshot FRAME]... [--screenshot-every N]
//                [--input SCRIPT] [--input-file PATH]
//
// --headless renders without a window (for screenshots); --no-video skips
// rendering entirely. F12 saves a screenshot to user/screenshots/.
// --input and --input-file play scripted input (see runtime/input/script.h).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "disc/disc.h"
#include "hw/vi.h"
#include "machine.h"
#include "os/crash.h"
#include "ppc/hooks.h"
#include "ppc/trace.h"
#include "ppc_recomp_shared.h"

#ifdef OPENGP_VIDEO
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "input/host.h"
#include "video/renderer.h"
#endif

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

std::atomic<unsigned long long> g_rendered{0};
std::atomic<unsigned long long> g_render_ns{0}, g_wait_ns{0};  // render thread: rendering, idle

unsigned long long elapsed_ns(std::chrono::steady_clock::time_point since) {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
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
  int timeout_s = -1, status_s = 5;
  unsigned scale = 1;
  bool headless = false, use_video = true;
  std::set<u64> screenshots;
  u64 screenshot_every = 0;
  input::Script script;
  std::string error;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iso" && i + 1 < argc) image = argv[++i];
    else if (a == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
    else if (a == "--status" && i + 1 < argc) status_s = std::atoi(argv[++i]);
    else if (a == "--scale" && i + 1 < argc) scale = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
    else if (a == "--screenshot" && i + 1 < argc) screenshots.insert(std::strtoull(argv[++i], nullptr, 10));
    else if (a == "--screenshot-every" && i + 1 < argc) screenshot_every = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--headless") headless = true;
    else if (a == "--no-video") use_video = false;
    else if ((a == "--input" || a == "--input-file") && i + 1 < argc) {
      std::string text = argv[++i];
      if (a == "--input-file") {
        std::ifstream f(text, std::ios::binary);
        if (!f) {
          std::fprintf(stderr, "cannot read %s\n", text.c_str());
          return 1;
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
      if (!script.parse(text, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
    } else {
      std::fprintf(stderr,
                   "usage: opengp-mkgp2 [--iso PATH] [--timeout SECONDS] [--status SECONDS] [--scale N]\n"
                   "                    [--headless] [--no-video] [--screenshot FRAME]... [--screenshot-every N]\n"
                   "                    [--input SCRIPT] [--input-file PATH]\n");
      return 2;
    }
  }
#ifndef OPENGP_VIDEO
  use_video = false;
#endif
  // With a window the game runs until it is closed; otherwise for 20 seconds.
  if (timeout_s < 0) timeout_s = (use_video && !headless) ? 0 : 20;
  if (image.empty()) image = find_image();
  if (image.empty()) {
    std::fprintf(stderr, "No %s image found in ISO/ or private/ (see docs/game-files.md), or pass --iso.\n",
                 kGameId);
    return 1;
  }

  static opengp::Machine machine;
  machine.set_user_dir((fs::path(OPENGP_SOURCE_DIR) / "user").string());
  machine.set_script(std::move(script));
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
    std::fprintf(stderr,
                 "[opengp] %s: %llu retraces, %llu frames (%llu rendered), %llu draws, %d guest threads, "
                 "PI cause %05X mask %05X\n",
                 why, static_cast<unsigned long long>(vi ? vi->retrace_count() : 0),
                 static_cast<unsigned long long>(machine.gpu().frames()), g_rendered.load(),
                 static_cast<unsigned long long>(machine.gpu().draws()), machine.thread_count(),
                 machine.hardware().interrupt_cause(), machine.hardware().interrupt_mask());
    const gx::Frontend::Stats& st = machine.gpu().frontend().stats();
    auto sec = [](const std::atomic<u64>& ns) { return static_cast<double>(ns.load()) * 1e-9; };
    std::fprintf(stderr,
                 "  guest: draws %.1f s (state %.1f s, textures %.1f s, %llu vertices %.1f s), %llu EFB readbacks "
                 "%.1f s; renderer: %.1f s, idle %.1f s; exceptions %llu (%llu unwound)\n",
                 sec(st.draw_ns), sec(st.state_ns), sec(st.texture_ns), static_cast<unsigned long long>(st.vertices.load()),
                 sec(st.transform_ns), static_cast<unsigned long long>(st.readbacks.load()), sec(st.readback_ns),
                 static_cast<double>(g_render_ns.load()) * 1e-9, static_cast<double>(g_wait_ns.load()) * 1e-9,
                 static_cast<unsigned long long>(machine.exceptions_delivered()),
                 static_cast<unsigned long long>(machine.exception_returns()));
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
  static const auto status_on_crash = print_status;
  os::install_crash_handler(machine.base(), [] { status_on_crash("crash"); });

  std::thread([timeout_s, status_s, print_status] {
    using clock = std::chrono::steady_clock;
    const auto deadline = timeout_s > 0 ? clock::now() + std::chrono::seconds(timeout_s) : clock::time_point::max();
    for (;;) {
      auto wake = deadline;
      if (status_s > 0) wake = std::min(wake, clock::now() + std::chrono::seconds(status_s));
      std::this_thread::sleep_until(wake);
      if (clock::now() >= deadline) {
        print_status("timeout");
        std::fflush(stdout);
        std::_Exit(0);
      }
      if (status_s > 0) print_status("status");
    }
  }).detach();

#ifdef OPENGP_VIDEO
  if (use_video) {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    SDL_Window* window = nullptr;
    if (!headless) {
      window = SDL_CreateWindow("OpenGP - Mario Kart Arcade GP 2", 1280, 960, SDL_WINDOW_RESIZABLE);
      if (!window) {
        std::fprintf(stderr, "could not create a window: %s\n", SDL_GetError());
        return 1;
      }
    }
    static video::Renderer renderer;
    if (!renderer.init(window, scale, &error)) {
      std::fprintf(stderr, "renderer: %s\n", error.c_str());
      return 1;
    }
    std::printf("  video:  %s, %ux internal resolution%s\n", renderer.driver(), scale, headless ? ", headless" : "");
    std::fflush(stdout);

    // The player's controls (not when headless: scripted input only).
    static input::HostInput host_input;
    bool use_input = false;
    if (!headless) {
      const std::string controls_path = (fs::path(OPENGP_SOURCE_DIR) / "user" / "controls.toml").string();
      if (!host_input.load(controls_path, &error)) std::fprintf(stderr, "[input] %s\n", error.c_str());
      use_input = host_input.init(&error);
      if (!use_input) std::fprintf(stderr, "[input] controllers unavailable: %s\n", error.c_str());
      std::printf("  input:  %s\n", controls_path.c_str());
    }

    static video::FrameQueue queue;
    machine.gpu().set_output(&queue);
    std::thread([] {
      machine.run();
      std::fprintf(stderr, "[opengp] entry point returned\n");
    }).detach();

    const fs::path shot_dir = fs::path(OPENGP_SOURCE_DIR) / "user" / "screenshots";
    bool shot_requested = false;
    for (;;) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) {
          print_status("window closed");
          std::fflush(stdout);
          std::_Exit(0);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F12) shot_requested = true;
        if (use_input) host_input.handle_event(e);
      }
      if (use_input) {
        machine.set_controls(host_input.poll());
        host_input.set_outputs(machine.outputs());
      }
      const auto wait_start = std::chrono::steady_clock::now();
      auto frame = queue.pop(8);
      g_wait_ns.fetch_add(elapsed_ns(wait_start));
      if (!frame) continue;
      const bool shoot = frame->xfb && (shot_requested || screenshots.count(frame->number) ||
                                        (screenshot_every && frame->number % screenshot_every == 0));
      video::Image shot;
      const auto render_start = std::chrono::steady_clock::now();
      renderer.render(*frame, shoot ? &shot : nullptr);
      g_render_ns.fetch_add(elapsed_ns(render_start));
      if (frame->xfb) g_rendered.fetch_add(1);
      const u64 number = frame->number;
      queue.give_back(std::move(frame));
      if (shoot && shot.width) {
        std::error_code ec;
        fs::create_directories(shot_dir, ec);
        const fs::path path = shot_dir / ("frame_" + std::to_string(number) + ".png");
        if (video::write_png(path.string(), shot)) std::fprintf(stderr, "[opengp] screenshot: %s\n", path.string().c_str());
        shot_requested = false;
      }
    }
  }
#endif

  machine.run();
  print_status("entry point returned");
  return 0;
}
