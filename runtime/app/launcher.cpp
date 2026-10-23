// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/launcher.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "app/hang_watch.h"
#include "app/paths.h"
#include "app/rich_presence.h"
#include "app/settings.h"
#include "audio/wav.h"
#include "camera/namcam.h"
#include "disc/disc.h"
#include "hw/dsp.h"
#include "hw/vi.h"
#include "input/script.h"
#include "machine.h"
#include "os/activity.h"
#include "os/atomic_file.h"
#include "os/crash.h"
#include "os/notice.h"
#include "os/printf.h"
#include "os/profiler.h"
#include "os/reboot.h"
#include "os/thread_priority.h"
#include "ppc/hooks.h"
#include "ppc/trace.h"

#ifdef OPENGP_VIDEO
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app/menu.h"
#include "audio/host.h"
#include "camera/webcam.h"
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
    "          [--input SCRIPT] [--input-file PATH] [--free-play] [--test-menu]\n"
    "          [--no-audio] [--volume PERCENT] [--audio-dump PATH] [--game-log]\n"
    "          [--camera off|webcam|test] [--cabinet 1-4] [--lan] [--version]\n";

struct Options {
  std::vector<std::string> args;  // as given, for restarts (without --reboot and --frame-offset)
  std::string reboot;             // file describing the reboot this run continues
  u64 frame_offset = 0;           // frames shown by the runs before the reboot
  bool test_menu = false;         // start in the game's test menu
  std::string image;
  std::string user_dir;  // UTF-8
  int timeout_s = -1, status_s = 5;
  bool headless = false, use_video = true, use_audio = true;
  bool game_log = false;  // print the game's debug messages
  std::set<u64> screenshots;
  u64 screenshot_every = 0;
  input::Script script;
  std::string audio_dump;
  // Settings for this run only.
  std::optional<bool> fullscreen, free_play;
  std::optional<AspectRatio> aspect;
  std::optional<u32> scale;
  std::optional<int> volume;
  std::optional<CameraSource> camera;
  std::optional<int> cabinet;
  std::optional<bool> lan;
};

// Returns 0 to go on, or an exit code.
int parse_options(const std::vector<std::string>& args, Options& o) {
  std::string error;
  const int argc = static_cast<int>(args.size());
  std::vector<const char*> argv_storage;
  for (const std::string& a : args) argv_storage.push_back(a.c_str());
  const char* const* argv = argv_storage.data();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "--reboot" || a == "--frame-offset") && i + 1 < argc) ++i;
    else if (a != "--test-menu") o.args.push_back(a);
  }
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--reboot" && has_value) {
      o.reboot = argv[++i];
      continue;
    }
    if (a == "--frame-offset" && has_value) {
      o.frame_offset = std::strtoull(argv[++i], nullptr, 10);
      continue;
    }
    if (a == "--iso" && has_value) o.image = argv[++i];
    else if (a == "--user-dir" && has_value) o.user_dir = argv[++i];
    else if (a == "--timeout" && has_value) o.timeout_s = std::atoi(argv[++i]);
    else if (a == "--status" && has_value) o.status_s = std::atoi(argv[++i]);
    else if (a == "--scale" && has_value) o.scale = static_cast<u32>(std::clamp(std::atoi(argv[++i]), 1, 8));
    else if (a == "--screenshot" && has_value) o.screenshots.insert(std::strtoull(argv[++i], nullptr, 10));
    else if (a == "--screenshot-every" && has_value) o.screenshot_every = std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--headless") o.headless = true;
    else if (a == "--test-menu") o.test_menu = true;
    else if (a == "--no-video") o.use_video = false;
    else if (a == "--fullscreen") o.fullscreen = true;
    else if (a == "--windowed") o.fullscreen = false;
    else if (a == "--free-play") o.free_play = true;
    else if (a == "--audio-dump" && has_value) o.audio_dump = argv[++i];
    else if (a == "--no-audio") o.use_audio = false;
    else if (a == "--game-log") o.game_log = true;
    else if (a == "--volume" && has_value) o.volume = std::clamp(std::atoi(argv[++i]), 0, 100);
    else if (a == "--lan") o.lan = true;
    else if (a == "--cabinet" && has_value) {
      const int n = std::atoi(argv[++i]);
      if (n < 1 || n > 4) {
        std::fprintf(stderr, "cabinet %s: give a number from 1 to 4\n", argv[i]);
        return 2;
      }
      o.cabinet = n;
    }
    else if (a == "--aspect" && has_value) {
      AspectRatio aspect;
      if (!parse_aspect(argv[++i], &aspect)) {
        std::fprintf(stderr, "unknown aspect ratio %s (auto, 4:3, 16:9, 16:10 or 21:9)\n", argv[i]);
        return 2;
      }
      o.aspect = aspect;
    } else if (a == "--camera" && has_value) {
      CameraSource camera;
      if (!parse_camera(argv[++i], &camera)) {
        std::fprintf(stderr, "unknown camera %s (off, webcam or test)\n", argv[i]);
        return 2;
      }
      o.camera = camera;
    } else if ((a == "--input" || a == "--input-file") && has_value) {
      std::string text = argv[++i];
      if (a == "--input-file") {
        std::ifstream f(os::utf8_path(text), std::ios::binary);
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
  if (o.camera) s.camera = *o.camera;
  if (o.cabinet) s.cabinet = *o.cabinet;
  if (o.lan) s.lan = *o.lan;
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
  OPENGP_KEEP_CHANGE(even_pacing);
  OPENGP_KEEP_CHANGE(volume);
  OPENGP_KEEP_CHANGE(free_play);
  OPENGP_KEEP_CHANGE(pause_in_menu);
  OPENGP_KEEP_CHANGE(pause_in_background);
  OPENGP_KEEP_CHANGE(card);
  OPENGP_KEEP_CHANGE(camera);
  OPENGP_KEEP_CHANGE(webcam);
  OPENGP_KEEP_CHANGE(rich_presence);
  OPENGP_KEEP_CHANGE(cabinet);
  OPENGP_KEEP_CHANGE(lan);
  OPENGP_KEEP_CHANGE(link_port);
#undef OPENGP_KEEP_CHANGE
  return s;
}

// The machine's network, so that the other cabinets hear when this one stops:
// the process ends with std::_Exit, without destructors.
triforce::Network* g_network = nullptr;
void stop_link() {
  if (g_network) g_network->stop_link();
}

// The player's devices, so that a wheel's forces stop with the process (it
// ends with std::_Exit, without destructors).
#ifdef OPENGP_VIDEO
input::HostInput* g_host_input = nullptr;
#endif
void stop_outputs() {
#ifdef OPENGP_VIDEO
  if (g_host_input) g_host_input->stop_outputs();
#endif
}

// The reboot the game asked for (set on the guest thread; the main thread
// restarts the process).
std::mutex g_reboot_mutex;
std::optional<os::Reboot> g_pending_reboot;
std::atomic<bool> g_reboot_requested{false};

// The reboot file (os/reboot.h): one per game, so that both games can run
// from one folder at once.
std::string g_reboot_file = "reboot.bin";

// Restarts the process to continue with `reboot` (see os/reboot.h).
// `frames` is how many frames have been shown since power-on.
[[noreturn]] void restart(const Options& o, const fs::path& user_dir, const char* argv0, const os::Reboot& reboot,
                          u64 frames) {
  // Messages the window can no longer show (a save just before the restart
  // failed): see quit().
  for (const std::string& notice : os::take_player_notices()) std::fprintf(stderr, "[unseen] %s\n", notice.c_str());
  const std::string path = os::utf8(user_dir / g_reboot_file);
  if (!os::save_reboot(path, reboot)) {
    std::fprintf(stderr, "[opengp] cannot write %s\n", path.c_str());
    std::_Exit(1);
  }
  stop_link();
  if (g_network) g_network->close_all();  // the other cabinets see the connections close
  stop_outputs();
  std::vector<std::string> args = o.args;
  args.push_back("--reboot");
  args.push_back(path);
  args.push_back("--frame-offset");
  args.push_back(std::to_string(frames));
  std::fprintf(stderr, "[opengp] restarting\n");
  os::restart_process(argv0, args);
}

// What a reboot the game asks for comes back up as. On a Triforce, games
// reboot through SegaBoot's system menu, which starts either the game's test
// menu or the game: Mario Kart GP 2 reboots with the same "test mode" memory
// both to enter its test menu (the test button) and to leave it (EXIT). So a
// reboot from a boot that kept memory (the test menu) boots afresh.
os::Reboot next_boot(const os::Reboot& requested) {
  if (os::g_previous_boot && !os::g_previous_boot->region.empty() && !requested.region.empty()) {
    os::Reboot fresh;
    fresh.restart = false;
    return fresh;
  }
  return requested;
}

// Whether start-up failures also show a message box: for a game started for a
// window, whose console (if any) may close with it. Not when the OpenGP
// launcher started it: the launcher shows the message from the log.
bool g_error_box = false;

// settings.toml could not be read nor set aside: it is never overwritten.
bool g_keep_settings_file = false;

// Reports a problem that stops the game from starting; returns the exit code.
int start_failed(const std::string& message) {
  // On one line in the log, whose last line the OpenGP launcher shows; the
  // box keeps the paragraphs.
  std::string line;
  for (const char c : message) {
    if (c != '\n') line += c;
    else if (!line.empty() && line.back() != ' ') line += ' ';
  }
  std::fprintf(stderr, "%s\n", line.c_str());
#ifdef OPENGP_VIDEO
  if (g_error_box) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenGP", message.c_str(), nullptr);
#endif
  return 1;
}

// The game's image: the one it was installed from (user/images.toml), else
// the first in ISO/ or private/ with its ID, else one holding its main.dol
// under another ID (a placeholder header).
std::string find_image(const Game& game, const fs::path& user_dir) {
  // The installer records it in the package's user/images.toml; another user
  // folder (--user-dir, for a second cabinet) may have its own.
  const fs::path dirs[] = {user_dir, home_dir() / "user"};
  for (std::size_t i = 0; i < 2; ++i) {
    if (i == 1 && dirs[1].lexically_normal() == dirs[0].lexically_normal()) break;  // the same folder
    if (std::string image = remembered_image(dirs[i], game.id); !image.empty()) {
      std::error_code ec;
      if (fs::is_regular_file(os::utf8_path(image), ec)) return image;
      std::fprintf(stderr, "[opengp] %s's image is no longer at %s: looking in ISO/ and private/\n", game.title,
                   image.c_str());
    }
  }
  std::vector<std::string> others;
  for (const char* folder : {"ISO", "private"}) {
    const fs::path dir = home_dir() / folder;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    // Never throws: an unreadable folder in there is skipped.
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      std::string ext = os::utf8(it->path().extension());
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      if (ext != ".iso" && ext != ".gcm") continue;
      const std::string path = os::utf8(it->path());
      disc::Image img;
      if (!img.open(path, nullptr)) continue;
      if (img.game_id() == game.id) return path;
      others.push_back(path);
    }
  }
  if (game.dol_sha1) {
    for (const std::string& path : others) {
      disc::Image img;
      if (img.open(path, nullptr) && disc::dol_sha1(img) == game.dol_sha1) return path;
    }
  }
  return {};
}

// The build runs only with the release of the game it was recompiled from: the
// image must hold the same executable. (The recompiled code needs the game's
// data and everything else from the image, which is not part of OpenGP.)
bool check_image(const Game& game, const std::string& path) {
  disc::Image img;
  std::string error;
  if (!img.open(path, &error)) {
    start_failed(error);
    return false;
  }
  const std::string actual = game.dol_sha1 ? disc::dol_sha1(img) : std::string();
  if (game.dol_sha1 && actual == game.dol_sha1) {
    if (img.game_id() != game.id)
      std::fprintf(stderr, "[opengp] %s says it is %s, but holds %s's main.dol: using it as %s\n", path.c_str(),
                   img.game_id().c_str(), game.id, game.id);
    return true;
  }
  if (img.game_id() != game.id) {
    start_failed(path + " is " + img.game_id() + ", not " + game.title + " (" + game.id + ").");
    return false;
  }
  if (!game.dol_sha1) return true;
  start_failed(path + ": its main.dol (SHA-1 " + (actual.empty() ? std::string("unreadable") : actual) +
               ") is not the release of " + game.title + " this build supports (SHA-1 " + game.dol_sha1 +
               "). See docs/game-files.md.");
  return false;
}

// Function names for backtraces, from the decomp-toolkit symbol file (UTF-8;
// an installed game's is relative to its executable's folder).
std::map<u32, std::string> load_function_names(const char* path) {
  std::map<u32, std::string> names;
  if (!path) return names;
  fs::path file = os::utf8_path(path);
  if (file.is_relative()) file = executable_dir() / file;
  std::ifstream f(file);
  if (!f) {
    std::fprintf(stderr, "[opengp] cannot read %s: reports show addresses without names\n", os::utf8(file).c_str());
    return names;
  }
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

// The camera's pictures for this run, or null for no camera: a webcam (with
// SDL; on the main thread) or the test pattern. `description` says which.
std::shared_ptr<camera::Source> start_camera(const Settings& s, std::string* description) {
  switch (s.camera) {
    case CameraSource::Off: break;
    case CameraSource::TestPattern: *description = "test pattern"; return std::make_shared<camera::TestPattern>();
    case CameraSource::Webcam: {
#ifdef OPENGP_VIDEO
      auto webcam = std::make_shared<camera::Webcam>();
      std::string error;
      if (webcam->choose(s.webcam, &error)) {
        *description = "webcam, " + webcam->name();
        return webcam;
      }
      std::fprintf(stderr, "[camera] %s: the game runs without its camera\n", error.c_str());
#else
      std::fprintf(stderr, "[camera] this build has no webcam support: the game runs without its camera\n");
#endif
      break;
    }
  }
  *description = "off";
  return nullptr;
}

unsigned long long elapsed_ns(std::chrono::steady_clock::time_point since) {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

}  // namespace

int run(const Game& game, int argc, char** argv) {
  const std::vector<std::string> args = os::utf8_args(argc, argv);
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--version") {
      std::printf("OpenGP %s: %s (%s)\n", OPENGP_VERSION, game.title, game.id);
      return 0;
    }
  }
  os::use_utf8_console();
  Options o;
  if (const int code = parse_options(args, o)) return code;
#ifndef OPENGP_VIDEO
  o.use_video = false;
#endif
  // With a window the game runs until it is closed; otherwise for 20 seconds.
  const bool windowed = o.use_video && !o.headless;
  if (o.timeout_s < 0) o.timeout_s = windowed ? 0 : 20;
  // Started by the OpenGP launcher (it sets OPENGP_LAUNCHER): the player has
  // no command line, so the messages point at the launcher instead.
  const bool from_launcher = std::getenv("OPENGP_LAUNCHER") != nullptr;
  g_error_box = windowed && !from_launcher;
  const fs::path user_dir = o.user_dir.empty() ? home_dir() / "user" : os::utf8_path(o.user_dir);
  g_reboot_file = std::string("reboot-") + game.id + ".bin";
  if (!lock_user_dir(user_dir, game.id)) {
    if (from_launcher)
      return start_failed(std::string(game.title) + " is already running from this OpenGP folder (" +
                          os::utf8(user_dir) + "). Close it before starting it again.");
    return start_failed(std::string("Another copy of ") + game.title + " is using the folder " + os::utf8(user_dir) +
                        ". Give each copy its own folder with --user-dir (see Linked cabinets in docs/playing.md).");
  }
  if (o.image.empty()) o.image = find_image(game, user_dir);
  if (o.image.empty()) {
    if (from_launcher)
      return start_failed(std::string("No disc image of ") + game.title + " (" + game.id +
                          ") found. In OpenGP, choose the game and add its disc image (Add disc image), or put the "
                          "image in the ISO folder next to OpenGP.");
    return start_failed(std::string("No disc image of ") + game.title + " (" + game.id +
                        ") found. Put it in the ISO folder next to OpenGP, or start the game with --iso PATH. If "
                        "you moved it, drop it on the OpenGP installer so the game knows where it is.");
  }
  if (!check_image(game, o.image)) return 1;

  // Settings: the file, then this run's options.
  std::string error;
  const std::string settings_path = os::utf8(user_dir / "settings.toml");
  static Settings file_settings;
  static std::string settings_problem;
  if (!load_settings(settings_path, &file_settings, &error)) {
    // Kept aside, not overwritten by the next save: the player can fix it.
    std::error_code ec;
    fs::rename(os::utf8_path(settings_path), os::utf8_path(settings_path + ".bad"), ec);
    if (ec) {
      g_keep_settings_file = true;  // never overwrite it
      settings_problem = error + ". The default settings are used, and changes are not saved until it is fixed.";
    } else {
      settings_problem = error + ". It was kept as settings.toml.bad; the default settings are used.";
    }
    file_settings = Settings{};
  } else {
    settings_problem = error;
  }
  if (!settings_problem.empty()) std::fprintf(stderr, "[settings] %s\n", settings_problem.c_str());
  static Settings settings = file_settings;
  apply_options(o, settings);
  static const Settings start_settings = settings;

  // A reboot to continue (the game restarted itself, see os/reboot.h).
  static os::Reboot previous_boot;
  if (!o.reboot.empty()) {
    if (os::load_reboot(o.reboot, &previous_boot)) os::g_previous_boot = &previous_boot;
    else std::fprintf(stderr, "[opengp] cannot read %s; booting afresh\n", o.reboot.c_str());
    std::error_code ec;
    fs::remove(os::utf8_path(o.reboot), ec);
  } else if (o.test_menu && game.test_menu) {
    previous_boot = game.test_menu();
    os::g_previous_boot = &previous_boot;
  }
  static opengp::Machine machine;
  machine.set_user_dir(os::utf8(user_dir));
  machine.set_script(std::move(o.script));
  machine.set_frame_offset(o.frame_offset);
  // A reboot now: with a window, the main thread restarts (after saving the
  // settings); without one, at once.
  static const Options* options = &o;
  static fs::path reboot_dir = user_dir;
  static const char* argv0 = argc ? argv[0] : "opengp";
  static bool main_thread_restarts = false;
  os::g_reboot_handler = [](const os::Reboot& r) {
    if (!main_thread_restarts)
      restart(*options, reboot_dir, argv0, next_boot(r), machine.frame_offset() + machine.gpu().frames());
    std::lock_guard lock(g_reboot_mutex);
    g_pending_reboot = r;
    g_reboot_requested = true;
  };

  os::BootResult boot;
  machine.set_game_id(game.id);
  if (!machine.boot(o.image, &boot, &error)) return start_failed("The game could not start: " + error);
  machine.install(*game.module);
  machine.cards().hold(settings.card);
  if (game.set_free_play) game.set_free_play(machine.base(), settings.free_play);
  if (game.set_game_log) game.set_game_log(o.game_log);
  static std::string camera_info;
  static std::shared_ptr<camera::Source> camera_source = start_camera(settings, &camera_info);
  if (camera_source) camera::attach(machine.network(), camera_source);
  if (game.set_camera) game.set_camera(camera_source != nullptr);
  machine.network().enable_link({game.id, static_cast<u16>(settings.link_port), settings.lan, OPENGP_VERSION});
  if (game.set_cabinet) {
    game.set_cabinet(settings.cabinet, [] {
      const triforce::LinkStatus link = machine.network().link_status();
      for (int n = 1; n <= 4; ++n)
        if (link.found[n] && !link.linked[n]) return true;
      return false;
    });
  }
  g_network = &machine.network();
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
  std::printf("OpenGP %s: %s (%s)\n", OPENGP_VERSION, game.title, machine.disc().game_id().c_str());
  std::printf("  image:  %s\n", o.image.c_str());
  std::printf("  memory: %u MiB, entry %08X, FST %08X, arena %08X-%08X\n", machine.mem1_size() >> 20,
              boot.entry_point, boot.fst_address, boot.arena_lo, boot.arena_hi);
  std::printf("  camera: %s\n", camera_info.c_str());
  std::fflush(stdout);

  static const auto names = load_function_names(game.symbols);
  auto print_status = [](const char* why, std::FILE* out = stderr) {
    auto* vi = machine.hardware().device<hw::ViDevice>();
    std::fprintf(out,
                 "[opengp] %s: %llu retraces, %llu frames (%llu rendered), %llu draws, %d guest threads, "
                 "PI cause %05X mask %05X\n",
                 why, static_cast<unsigned long long>(vi ? vi->retrace_count() : 0),
                 static_cast<unsigned long long>(machine.gpu().frames()), g_rendered.load(),
                 static_cast<unsigned long long>(machine.gpu().draws()), machine.thread_count(),
                 machine.hardware().interrupt_cause(), machine.hardware().interrupt_mask());
    const gx::Frontend::Stats& st = machine.gpu().frontend().stats();
    auto sec = [](const std::atomic<u64>& ns) { return static_cast<double>(ns.load()) * 1e-9; };
    std::fprintf(out,
                 "  guest: draws %.1f s (state %.1f s, textures %.1f s, %llu vertices %.1f s), %llu EFB readbacks "
                 "%.1f s; renderer: %.1f s, idle %.1f s; exceptions %llu (%llu unwound)\n",
                 sec(st.draw_ns), sec(st.state_ns), sec(st.texture_ns), static_cast<unsigned long long>(st.vertices.load()),
                 sec(st.transform_ns), static_cast<unsigned long long>(st.readbacks.load()), sec(st.readback_ns),
                 static_cast<double>(g_render_ns.load()) * 1e-9, static_cast<double>(g_wait_ns.load()) * 1e-9,
                 static_cast<unsigned long long>(machine.exceptions_delivered()),
                 static_cast<unsigned long long>(machine.exception_returns()));
#ifdef OPENGP_VIDEO
    if (g_audio_out)
      std::fprintf(out, "  audio: queue %.0f ms, %llu underruns, %llu overflows\n", g_audio_out->queue_ms(),
                   static_cast<unsigned long long>(g_audio_out->underruns()),
                   static_cast<unsigned long long>(g_audio_out->overflows()));
#endif
    for (ppc::TraceStack* s : ppc::trace_all_stacks()) {
      const auto frames = ppc::trace_snapshot(*s);
      if (frames.empty()) continue;
      std::fprintf(out, "  stack:");
      const std::size_t first = frames.size() > 12 ? frames.size() - 12 : 0;
      for (std::size_t i = frames.size(); i-- > first;) std::fprintf(out, "\n    %s", describe(names, frames[i]).c_str());
      std::fprintf(out, "\n");
    }
    std::fflush(out);
  };
  // Crashes, fatal errors and uncaught exceptions end in a crash report:
  // user/crash-report.txt and the console, with the guest threads' stacks.
  static const auto status_on_crash = print_status;
  const std::string header = std::string("OpenGP ") + OPENGP_VERSION + ", " + game.title + " (" + game.id + ")";
  os::install_crash_handler(
      machine.base(),
      [](std::FILE* out) {
        if (out == stderr) {
          stop_link();
          stop_outputs();
        }
        status_on_crash("crash", out);
      },
      os::utf8(user_dir / "crash-report.txt"), header);
  ppc::g_hooks.fatal = [](const char* message) {
    char reason[512];
    std::snprintf(reason, sizeof(reason), "fatal: %s", message);
    os::crash_now(reason);
  };
  // Hang reports, from when the game starts running: a message about a
  // start-up failure can stay open for as long as the player likes.
  auto watch_for_hangs = [&user_dir, &header] {
    app::start_hang_watch(machine, names, user_dir / "hang-report.txt", header);
  };
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
        stop_link();
        std::fflush(stdout);
        std::_Exit(0);
      }
      if (status_s > 0) print_status("status");
    }
  }).detach();

#ifdef OPENGP_VIDEO
  if (o.use_video) {
    main_thread_restarts = true;
    SDL_SetMainReady();
    if (!SDL_Init(SDL_INIT_VIDEO)) return start_failed(std::string("SDL_Init failed: ") + SDL_GetError());
    // Closing the console window quits as closing the game's window does:
    // the settings are saved and the other cabinets told.
    os::on_console_close([] {
      SDL_Event quit_event{};
      quit_event.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&quit_event);
    });
    SDL_Window* window = nullptr;
    if (!o.headless) {
      const std::string title = std::string("OpenGP - ") + game.title;
      window = SDL_CreateWindow(title.c_str(), settings.window_width, settings.window_height,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                    (settings.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
      if (!window) return start_failed(std::string("Could not create a window: ") + SDL_GetError());
    }
    static video::Renderer renderer;
    if (!renderer.init(window, internal_scale(settings.internal_resolution, 0), &error)) {
      return start_failed("The game cannot draw: " + error +
                          "\n\nOpenGP needs a graphics card and driver with Vulkan (Windows and Linux) or Metal "
                          "(macOS). Updating the graphics driver often helps.");
    }
    std::printf("  video:  %s%s\n", renderer.driver(), o.headless ? ", headless" : "");
    std::fflush(stdout);

    // The player's controls, sound and menu (not when headless: scripted
    // input only, no sound).
    static input::HostInput host_input;
    g_host_input = &host_input;
    bool use_input = false;
    static Menu menu;
    bool use_menu = false;
    if (!o.headless) {
      const std::string controls_path = os::utf8(user_dir / "controls.toml");
      error.clear();
      host_input.load(controls_path, &error);
      if (!error.empty()) os::notify_player(error);
      if (!settings_problem.empty()) os::notify_player(settings_problem);
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
    watch_for_hangs();  // once: the loop below never ends, it quits or restarts
    std::thread([] {
      os::set_interactive_thread();
      machine.run();
      std::fprintf(stderr, "[opengp] entry point returned\n");
    }).detach();

    auto save = [&settings_path, &use_input] {
      if (use_input) {
        std::string why;
        if (!host_input.save(&why)) os::notify_player("The controls could not be saved: " + why);
      }
      if (settings == start_settings || g_keep_settings_file) return;
      const Settings s = settings_to_save(file_settings, start_settings, settings);
      if (s == file_settings) return;
      std::string why;
      if (save_settings(settings_path, s, &why)) file_settings = s;
      else os::notify_player("The settings could not be saved: " + why);
    };
    auto quit = [&save, &print_status](const char* why) {
      save();
      // What the player can no longer see in the window (a save that failed
      // just now): for the console, and for the launcher, which shows lines
      // that start "[unseen]" once the game has ended.
      for (const std::string& notice : os::take_player_notices()) std::fprintf(stderr, "[unseen] %s\n", notice.c_str());
      print_status(why);
      stop_link();
      stop_outputs();
      std::fflush(stdout);
      // Closed while the crash message was up (Windows): still a crash.
      std::_Exit(os::crashing() ? 3 : 0);
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
    bool pause_pressed = false;  // paused with the pause control
    // Discord Rich Presence: when playing in a window (not in headless runs,
    // unless OPENGP_LOG_DISCORD=1 asks for what it would show).
    const bool log_discord = std::getenv("OPENGP_LOG_DISCORD") != nullptr;
    std::unique_ptr<RichPresence> rich_presence;
    auto next_status = std::chrono::steady_clock::now();
    auto next_link_check = next_status;
    std::optional<std::chrono::steady_clock::time_point> link_lost;  // when the game stopped with a link error
    MenuModel model;
    model.title = game.title;
    model.settings = &settings;
    model.cards = &machine.cards();
    model.input = use_input ? &host_input : nullptr;
    model.camera_info = camera_info;
    model.webcams = [] { return camera::Webcam::names(); };
    model.quit = [&quit] { quit("quit from the menu"); };
    // Even frame pacing: the retraces that say when to show each frame, and
    // when the last one was shown (steady clock, ns).
    hw::ViDevice* const vi = machine.hardware().device<hw::ViDevice>();
    u64 shown_ns = 0;
    if (game.test_menu) {
      model.test_menu = [&save, &o, &user_dir, &game, argv] {
        save();
        restart(o, user_dir, argv[0], game.test_menu(), machine.frame_offset() + machine.gpu().frames());
      };
    }
    bool menu_was_open = false;
    for (;;) {
      if (g_reboot_requested) {
        save();
        std::lock_guard lock(g_reboot_mutex);
        restart(o, user_dir, argv[0], next_boot(*g_pending_reboot), machine.frame_offset() + machine.gpu().frames());
      }
      if (use_menu) {
        for (const std::string& notice : os::take_player_notices()) menu.notify(notice);
        // The settings are saved when the menu closes, whatever closed it
        // (Esc, the Guide button, or its Resume button).
        if (menu_was_open && !menu.is_open()) save();
        menu_was_open = menu.is_open();
      }
      os::g_main_activity.set("handling the window's events");
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
        // Choosing an input for a control in the menu takes the next one.
        if (use_input && host_input.capturing() && host_input.capture_event(e)) continue;
        if (use_menu && menu.handle_event(e)) continue;
        if (use_input) host_input.handle_event(e);
      }
      // Pausing: with the pause control, while the menu is open, or while the
      // window is in the background (as the player chose). Only when playing
      // solo: once the game has linked to another cabinet, the others need it
      // to keep running.
      const bool linked = machine.network().linked();
      if (use_input && host_input.take_pause_press() && !menu.is_open()) {
        if (linked) menu.notify("A linked game can't pause");
        else pause_pressed = !pause_pressed;
      }
      if (linked) pause_pressed = false;
      model.linked = linked;
      model.link = machine.network().link_status();
      const SDL_WindowFlags window_flags = window ? SDL_GetWindowFlags(window) : 0;
      const bool background = window && ((window_flags & SDL_WINDOW_MINIMIZED) || !(window_flags & SDL_WINDOW_INPUT_FOCUS));
      const bool paused = !linked && (pause_pressed || (menu.is_open() && settings.pause_in_menu) ||
                                      (background && settings.pause_in_background));
      if (paused != machine.hardware().paused()) {
        machine.hardware().set_paused(paused);
        if (g_audio_out) g_audio_out->set_paused(paused);
      }
      menu.set_paused(paused);
      if (use_input) {
        // While the menu is open the game gets nothing (the menu has the keys).
        machine.set_controls(menu.is_open() ? triforce::Controls{} : host_input.poll());
        // A paused game's wheel forces stop too.
        host_input.set_outputs(paused ? triforce::Outputs{} : machine.outputs());
      }
      settings.card = applied.card = machine.cards().held();  // the game may have given the player a new card
      apply_settings();
      if (!settings.rich_presence || !game.status || !(window || log_discord)) {
        rich_presence.reset();
      } else if (const auto now = std::chrono::steady_clock::now(); now >= next_status) {
        next_status = now + std::chrono::seconds(1);
        if (!rich_presence) rich_presence = std::make_unique<RichPresence>(game.title, log_discord);
        GameStatus status;
        game.status(machine.base(), &status);
        rich_presence->update(status);
      }
      model.discord = rich_presence && rich_presence->connected();
      // A linked game that loses its link (another cabinet stopped) stops
      // with a link error until the cabinet is restarted, as an attendant
      // would: OpenGP restarts it.
      if (const auto now = std::chrono::steady_clock::now(); linked && game.link_error && now >= next_link_check) {
        next_link_check = now + std::chrono::milliseconds(500);
        if (!game.link_error(machine.base())) {
          link_lost.reset();
        } else if (!link_lost) {
          link_lost = now;
          menu.notify("The link to the other cabinets was lost: restarting");
          std::fprintf(stderr, "[opengp] the link to the other cabinets was lost: restarting\n");
        } else if (now - *link_lost >= std::chrono::seconds(4)) {
          save();
          os::Reboot fresh;
          fresh.restart = false;
          restart(o, user_dir, argv[0], fresh, machine.frame_offset() + machine.gpu().frames());
        }
      }
      if (window) {
        const bool hide = settings.fullscreen && !menu.is_open();
        if (hide != cursor_hidden) {
          if (hide) SDL_HideCursor();
          else SDL_ShowCursor();
          cursor_hidden = hide;
        }
      }

      const auto wait_start = std::chrono::steady_clock::now();
      os::g_main_activity.set("waiting for the game's next frame");
      auto frame = queue.pop(8);
      g_wait_ns.fetch_add(elapsed_ns(wait_start));
      if (!frame) {
        // The game has shown nothing new for a while (paused, loading, or
        // stuck): keep the menu alive.
        const auto redraw_after = std::chrono::milliseconds(paused ? 16 : 100);
        if (use_menu && menu.visible() && std::chrono::steady_clock::now() - last_present > redraw_after) {
          menu.update(model);
          renderer.redraw();
          last_present = std::chrono::steady_clock::now();
        }
        continue;
      }
      // Frames count from power-on, across the game's reboots.
      const u64 number = o.frame_offset + frame->number;
      const bool shoot = frame->xfb && (shot_requested || o.screenshots.count(number) ||
                                        (o.screenshot_every && number % o.screenshot_every == 0));
      if (use_menu && frame->xfb) {
        const u32 scale = renderer.scale();
        model.video_info = std::string(renderer.driver()) + ", " + std::to_string(frame->efb_width * scale) + "x" +
                           std::to_string(gx::kEfbHeight * scale) + " (" + std::to_string(scale) + "x)";
        menu.update(model);
      }
      video::Image shot;
      // With even frame pacing a frame is shown at the game's first retrace
      // after it was finished, as the display would show its XFB, instead of
      // as soon as it is drawn: the game's frames take more or less time, but
      // its retraces are evenly spaced, and a display with a variable refresh
      // rate (FreeSync, G-Sync) shows each frame as it comes. A frame whose
      // retrace has gone by (it is late) is shown at once.
      const bool paced = settings.even_pacing && window && frame->xfb && vi;
      const auto render_start = std::chrono::steady_clock::now();
      os::g_main_activity.set("drawing a frame");
      renderer.render(*frame, shoot ? &shot : nullptr, !paced);
      g_render_ns.fetch_add(elapsed_ns(render_start));
      if (paced) {
        // After the last frame's retrace too: one frame a retrace.
        const u64 due = vi->next_retrace_ns(std::max(frame->finished_ns, shown_ns));
        g_wait_ns.fetch_add(renderer.redraw(due));
        if (due) shown_ns = due;
      }
      if (frame->xfb) {
        g_rendered.fetch_add(1);
        last_present = std::chrono::steady_clock::now();
      }
      queue.give_back(std::move(frame));
      if (shoot && shot.width) {
        std::error_code ec;
        fs::create_directories(shot_dir, ec);
        const fs::path path = shot_dir / ("frame_" + std::to_string(number) + ".png");
        if (video::write_png(os::utf8(path), shot)) {
          std::fprintf(stderr, "[opengp] screenshot: %s\n", os::utf8(path).c_str());
          if (shot_requested && use_menu) menu.notify("Screenshot saved: " + os::utf8(path.filename()));
        } else if (shot_requested) {
          os::notify_player("The screenshot could not be saved to " + os::utf8(shot_dir));
        }
        shot_requested = false;
      }
    }
  }
#endif

  watch_for_hangs();
  machine.run();
  print_status("entry point returned");
  return 0;
}

}  // namespace app
