// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/launcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "app/settings.h"
#include "audio/wav.h"
#include "disc/disc.h"
#include "hw/dsp.h"
#include "hw/vi.h"
#include "input/script.h"
#include "machine.h"
#include "os/crash.h"
#include "os/profiler.h"
#include "ppc/hooks.h"
#include "ppc/trace.h"

#ifdef OPENGP_VIDEO
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/menu.h"
#include "audio/host.h"
#include "input/host.h"
#include "video/renderer.h"
#endif

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr const char* kUsage =
    "usage: %s [--iso PATH] [--user-dir PATH] [--timeout SECONDS] [--status SECONDS]\n"
    "          [--fullscreen | --windowed] [--aspect auto|4:3|16:9|16:10|21:9] [--scale N]\n"
    "          [--headless] [--no-video] [--screenshot FRAME]... [--screenshot-every N]\n"
    "          [--input SCRIPT] [--input-file PATH] [--free-play]\n"
    "          [--no-audio] [--volume PERCENT] [--audio-dump PATH]\n";

struct Options {
  std::string image;
  fs::path user_dir;
  int timeout_s = -1, status_s = 5;
  bool headless = false, use_video = true, use_audio = true;
  std::set<u64> screenshots;
  u64 screenshot_every = 0;
  input::Script script;
  std::string audio_dump;
  // Settings for this run only.
  std::optional<bool> fullscreen, free_play;
  std::optional<AspectRatio> aspect;
  std::optional<u32> scale;
  std::optional<int> volume;
};

// Returns 0 to go on, or an exit code.
int parse_options(int argc, char** argv, Options& o) {
  std::string error;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--iso" && has_value) o.image = argv[++i];
    else if (a == "--user-dir" && has_value) o.user_dir = argv[++i];
    else if (a == "--timeout" && has_value) o.timeout_s = std::atoi(argv[++i]);
    else if (a == "--status" && has_value) o.status_s = std::atoi(argv[++i]);
    else if (a == "--scale" && has_value) o.scale = static_cast<u32>(std::clamp(std::atoi(argv[++i]), 1, 8));
    else if (a == "--screenshot" && has_value) o.screenshots.insert(std::strtoull(argv[++i], nullptr, 10));
    else if (a == "--screenshot-every" && has_value) o.screenshot_every = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--headless") o.headless = true;
    else if (a == "--no-video") o.use_video = false;
    else if (a == "--fullscreen") o.fullscreen = true;
    else if (a == "--windowed") o.fullscreen = false;
    else if (a == "--free-play") o.free_play = true;
    else if (a == "--audio-dump" && has_value) o.audio_dump = argv[++i];
    else if (a == "--no-audio") o.use_audio = false;
    else if (a == "--volume" && has_value) o.volume = std::clamp(std::atoi(argv[++i]), 0, 100);
    else if (a == "--aspect" && has_value) {
      AspectRatio aspect;
      if (!parse_aspect(argv[++i], &aspect)) {
        std::fprintf(stderr, "unknown aspect ratio %s (auto, 4:3, 16:9, 16:10 or 21:9)\n", argv[i]);
        return 2;
      }
      o.aspect = aspect;
    } else if ((a == "--input" || a == "--input-file") && has_value) {
      std::string text = argv[++i];
      if (a == "--input-file") {
        std::ifstream f(text, std::ios::binary);
        if (!f) {
          std::fprintf(stderr, "cannot read %s\n", text.c_str());
          return 1;
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
      }
      if (!o.script.parse(text, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
      }
    } else {
      std::fprintf(stderr, kUsage, argc ? argv[0] : "opengp");
      return 2;
    }
  }
  return 0;
}

void apply_options(const Options& o, Settings& s) {
  if (o.fullscreen) s.fullscreen = *o.fullscreen;
  if (o.free_play) s.free_play = *o.free_play;
  if (o.aspect) s.aspect = *o.aspect;
  if (o.scale) s.internal_resolution = *o.scale;
  if (o.volume) s.volume = *o.volume;
}

// What to save: the settings file's values, except those changed (in the
// menu, or by resizing the window) since start-up. Command-line options stay
// out of the file.
Settings settings_to_save(const Settings& file, const Settings& start, const Settings& now) {
  Settings s = file;
#define OPENGP_KEEP_CHANGE(field) \
  if (now.field != start.field) s.field = now.field
  OPENGP_KEEP_CHANGE(fullscreen);
  OPENGP_KEEP_CHANGE(window_width);
  OPENGP_KEEP_CHANGE(window_height);
  OPENGP_KEEP_CHANGE(aspect);
  OPENGP_KEEP_CHANGE(internal_resolution);
  OPENGP_KEEP_CHANGE(vsync);
  OPENGP_KEEP_CHANGE(volume);
  OPENGP_KEEP_CHANGE(free_play);
  OPENGP_KEEP_CHANGE(card);
#undef OPENGP_KEEP_CHANGE
  return s;
}

// Looks for the game's image in ISO/ and private/.
std::string find_image(const char* game_id) {
  for (const char* folder : {"ISO", "private"}) {
    const fs::path dir = fs::path(OPENGP_SOURCE_DIR) / folder;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
      const std::string ext = entry.path().extension().string();
      if (ext != ".iso" && ext != ".gcm" && ext != ".ISO" && ext != ".GCM") continue;
      disc::Image img;
      if (img.open(entry.path().string(), nullptr) && img.game_id() == game_id) return entry.path().string();
    }
  }
  return {};
}

// Function names for backtraces, from the decomp-toolkit symbol file.
std::map<u32, std::string> load_function_names(const char* path) {
  std::map<u32, std::string> names;
  if (!path) return names;
  std::ifstream f(path);
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

#ifdef OPENGP_VIDEO
audio::HostOutput* g_audio_out = nullptr;
#endif
audio::WavWriter* g_audio_dump = nullptr;

std::atomic<unsigned long long> g_rendered{0};
std::atomic<unsigned long long> g_render_ns{0}, g_wait_ns{0};  // render thread: rendering, idle

unsigned long long elapsed_ns(std::chrono::steady_clock::time_point since) {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}  // namespace

int run(const Game& game, int argc, char** argv) {
  Options o;
  if (const int code = parse_options(argc, argv, o)) return code;
#ifndef OPENGP_VIDEO
  o.use_video = false;
#endif
  // With a window the game runs until it is closed; otherwise for 20 seconds.
  const bool windowed = o.use_video && !o.headless;
  if (o.timeout_s < 0) o.timeout_s = windowed ? 0 : 20;
  if (o.image.empty()) o.image = find_image(game.id);
  if (o.image.empty()) {
    std::fprintf(stderr, "No %s image found in ISO/ or private/ (see docs/game-files.md), or pass --iso.\n", game.id);
    return 1;
  }
  const fs::path user_dir = o.user_dir.empty() ? fs::path(OPENGP_SOURCE_DIR) / "user" : o.user_dir;

  // Settings: the file, then this run's options.
  std::string error;
  const std::string settings_path = (user_dir / "settings.toml").string();
  static Settings file_settings;
  if (!load_settings(settings_path, &file_settings, &error)) {
    std::fprintf(stderr, "[settings] %s (using the defaults)\n", error.c_str());
    file_settings = Settings{};
  }
  static Settings settings = file_settings;
  apply_options(o, settings);
  static const Settings start_settings = settings;

  static opengp::Machine machine;
  machine.set_user_dir(user_dir.string());
  machine.set_script(std::move(o.script));
  os::BootResult boot;
  if (!machine.boot(o.image, &boot, &error)) {
    std::fprintf(stderr, "boot failed: %s\n", error.c_str());
    return 1;
  }
  machine.install(*game.module);
  machine.cards().hold(settings.card);
  if (game.set_free_play) game.set_free_play(machine.base(), settings.free_play);
  machine.gpu().frontend().set_widescreen(widescreen_side(settings.aspect, 0, 0));

  static audio::WavWriter wav;
  if (!o.audio_dump.empty()) {
    if (!wav.open(o.audio_dump)) {
      std::fprintf(stderr, "cannot write %s\n", o.audio_dump.c_str());
      return 1;
    }
    g_audio_dump = &wav;
  }
  machine.hardware().device<hw::DspDevice>()->set_audio_sink([](const s16* samples, u32 frames, u32 rate) {
#ifdef OPENGP_VIDEO
    if (g_audio_out) g_audio_out->push(samples, frames, rate);
#endif
    if (g_audio_dump) g_audio_dump->write(samples, frames, rate);
  });
  std::printf("OpenGP: %s (%s)\n", game.title, machine.disc().game_id().c_str());
  std::printf("  image:  %s\n", o.image.c_str());
  std::printf("  memory: %u MiB, entry %08X, FST %08X, arena %08X-%08X\n", machine.mem1_size() >> 20,
              boot.entry_point, boot.fst_address, boot.arena_lo, boot.arena_hi);
  std::fflush(stdout);

  static const auto names = load_function_names(game.symbols);
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
#ifdef OPENGP_VIDEO
    if (g_audio_out)
      std::fprintf(stderr, "  audio: queue %.0f ms, %llu underruns, %llu overflows\n", g_audio_out->queue_ms(),
                   static_cast<unsigned long long>(g_audio_out->underruns()),
                   static_cast<unsigned long long>(g_audio_out->overflows()));
#endif
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
  ppc::g_hooks.fatal = [](const char* message) { std::fprintf(stderr, "[opengp] fatal: %s\n", message); };
  static const auto status_on_crash = print_status;
  os::install_crash_handler(machine.base(), [] { status_on_crash("crash"); });
  os::start_profiler_from_env();

  std::thread([timeout_s = o.timeout_s, status_s = o.status_s, print_status] {
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
  if (o.use_video) {
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    SDL_Window* window = nullptr;
    if (!o.headless) {
      const std::string title = std::string("OpenGP - ") + game.title;
      window = SDL_CreateWindow(title.c_str(), settings.window_width, settings.window_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                    (settings.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
      if (!window) {
        std::fprintf(stderr, "could not create a window: %s\n", SDL_GetError());
        return 1;
      }
    }
    static video::Renderer renderer;
    if (!renderer.init(window, internal_scale(settings.internal_resolution, 0), &error)) {
      std::fprintf(stderr, "renderer: %s\n", error.c_str());
      return 1;
    }
    std::printf("  video:  %s%s\n", renderer.driver(), o.headless ? ", headless" : "");
    std::fflush(stdout);

    // The player's controls, sound and menu (not when headless: scripted
    // input only, no sound).
    static input::HostInput host_input;
    bool use_input = false;
    static Menu menu;
    bool use_menu = false;
    if (!o.headless) {
      const std::string controls_path = (user_dir / "controls.toml").string();
      if (!host_input.load(controls_path, &error)) std::fprintf(stderr, "[input] %s\n", error.c_str());
      use_input = host_input.init(&error);
      if (!use_input) std::fprintf(stderr, "[input] controllers unavailable: %s\n", error.c_str());
      std::printf("  input:  %s\n", controls_path.c_str());
      use_menu = menu.init(window, renderer.device(), renderer.window_format(), &error);
      if (use_menu) renderer.set_overlay(&menu);
      else std::fprintf(stderr, "[menu] %s\n", error.c_str());
    }
    static audio::HostOutput audio_out;
    if (!o.headless && o.use_audio) {
      if (audio_out.init(static_cast<float>(settings.volume) / 100.0f, &error)) {
        g_audio_out = &audio_out;
        std::printf("  audio:  %s\n", audio_out.device_name());
      } else {
        std::fprintf(stderr, "[audio] no sound: %s\n", error.c_str());
      }
    }
    std::printf("  saves:  %s\n", machine.cards().dir().c_str());
    std::fflush(stdout);

    static video::FrameQueue queue;
    machine.gpu().set_output(&queue);
    std::thread([] {
      machine.run();
      std::fprintf(stderr, "[opengp] entry point returned\n");
    }).detach();

    auto save = [&settings_path] {
      if (settings == start_settings) return;
      const Settings s = settings_to_save(file_settings, start_settings, settings);
      if (s == file_settings) return;
      std::string why;
      if (save_settings(settings_path, s, &why)) file_settings = s;
      else std::fprintf(stderr, "[settings] %s\n", why.c_str());
    };
    auto quit = [&save, &print_status](const char* why) {
      save();
      print_status(why);
      std::fflush(stdout);
      std::_Exit(0);
    };

    // Applies settings that changed (in the menu, by resizing the window...).
    Settings applied = settings;
    int applied_width = -1, applied_height = -1;
    auto apply_settings = [&] {
      int pw = 0, ph = 0;
      if (window) SDL_GetWindowSizeInPixels(window, &pw, &ph);
      if (settings == applied && pw == applied_width && ph == applied_height) return;
      if (window && settings.fullscreen != applied.fullscreen) SDL_SetWindowFullscreen(window, settings.fullscreen);
      if (settings.vsync != applied.vsync || applied_width < 0) renderer.set_vsync(settings.vsync);
      if (g_audio_out && settings.volume != applied.volume) g_audio_out->set_volume(static_cast<float>(settings.volume) / 100.0f);
      if (game.set_free_play && settings.free_play != applied.free_play) game.set_free_play(machine.base(), settings.free_play);
      if (settings.card != applied.card) machine.cards().hold(settings.card);
      machine.gpu().frontend().set_widescreen(widescreen_side(settings.aspect, pw, ph));
      renderer.set_scale(internal_scale(settings.internal_resolution, ph));
      applied = settings;
      applied_width = pw, applied_height = ph;
    };
    apply_settings();

    const fs::path shot_dir = user_dir / "screenshots";
    bool shot_requested = false;
    bool cursor_hidden = false;
    auto last_present = std::chrono::steady_clock::now();
    MenuModel model;
    model.title = game.title;
    model.settings = &settings;
    model.cards = &machine.cards();
    model.quit = [&quit] { quit("quit from the menu"); };
    for (;;) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        switch (e.type) {
          case SDL_EVENT_QUIT: quit("window closed"); break;
          case SDL_EVENT_WINDOW_RESIZED:
            // Remember the window's size (not the fullscreen or maximised one).
            if (window && !(SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
              settings.window_width = e.window.data1;
              settings.window_height = e.window.data2;
              applied.window_width = e.window.data1;
              applied.window_height = e.window.data2;
            }
            break;
          case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: settings.fullscreen = applied.fullscreen = true; break;
          case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: settings.fullscreen = applied.fullscreen = false; break;
          case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat) break;
            if (e.key.key == SDLK_F12) shot_requested = true;
            if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
              settings.fullscreen = !settings.fullscreen;
              continue;  // not for the game (Enter is its start button)
            }
            break;
          default: break;
        }
        const bool was_open = menu.is_open();
        if (use_menu && menu.handle_event(e)) {
          if (was_open && !menu.is_open()) save();
          continue;
        }
        if (use_input) host_input.handle_event(e);
      }
      if (use_input) {
        // While the menu is open the game gets nothing (the menu has the keys).
        machine.set_controls(menu.is_open() ? triforce::Controls{} : host_input.poll());
        host_input.set_outputs(machine.outputs());
      }
      settings.card = applied.card = machine.cards().held();  // the game may have given the player a new card
      apply_settings();
      if (window) {
        const bool hide = settings.fullscreen && !menu.is_open();
        if (hide != cursor_hidden) {
          if (hide) SDL_HideCursor();
          else SDL_ShowCursor();
          cursor_hidden = hide;
        }
      }

      const auto wait_start = std::chrono::steady_clock::now();
      auto frame = queue.pop(8);
      g_wait_ns.fetch_add(elapsed_ns(wait_start));
      if (!frame) {
        // The game has shown nothing new for a while (loading, or stuck):
        // keep the menu alive.
        if (use_menu && menu.visible() && std::chrono::steady_clock::now() - last_present > std::chrono::milliseconds(100)) {
          menu.update(model);
          renderer.redraw();
          last_present = std::chrono::steady_clock::now();
        }
        continue;
      }
      const bool shoot = frame->xfb && (shot_requested || o.screenshots.count(frame->number) ||
                                        (o.screenshot_every && frame->number % o.screenshot_every == 0));
      if (use_menu && frame->xfb) {
        const u32 scale = renderer.scale();
        model.video_info = std::string(renderer.driver()) + ", " + std::to_string(frame->efb_width * scale) + "x" +
                           std::to_string(gx::kEfbHeight * scale) + " (" + std::to_string(scale) + "x)";
        menu.update(model);
      }
      video::Image shot;
      const auto render_start = std::chrono::steady_clock::now();
      renderer.render(*frame, shoot ? &shot : nullptr);
      g_render_ns.fetch_add(elapsed_ns(render_start));
      if (frame->xfb) {
        g_rendered.fetch_add(1);
        last_present = std::chrono::steady_clock::now();
      }
      const u64 number = frame->number;
      queue.give_back(std::move(frame));
      if (shoot && shot.width) {
        std::error_code ec;
        fs::create_directories(shot_dir, ec);
        const fs::path path = shot_dir / ("frame_" + std::to_string(number) + ".png");
        if (video::write_png(path.string(), shot)) {
          std::fprintf(stderr, "[opengp] screenshot: %s\n", path.string().c_str());
          if (shot_requested && use_menu) menu.notify("Screenshot saved: " + path.filename().string());
        }
        shot_requested = false;
      }
    }
  }
#endif

  machine.run();
  print_status("entry point returned");
  return 0;
}

}  // namespace app
