// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/settings.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <utility>

#include "os/atomic_file.h"

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace app {

namespace {

struct AspectInfo {
  AspectRatio aspect;
  const char* name;
  double ratio;  // width / height; 0 for the window's
};
constexpr AspectInfo kAspects[] = {
    {AspectRatio::Auto, "auto", 0.0},           {AspectRatio::Original, "4:3", 4.0 / 3.0},
    {AspectRatio::Wide16x9, "16:9", 16.0 / 9.0}, {AspectRatio::Wide16x10, "16:10", 16.0 / 10.0},
    {AspectRatio::Wide21x9, "21:9", 64.0 / 27.0},
};

// TOML string escaping for the few strings written (card names).
std::string quoted(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    out += c;
  }
  return out + "\"";
}

constexpr std::pair<CameraSource, const char*> kCameras[] = {
    {CameraSource::Off, "off"}, {CameraSource::Webcam, "webcam"}, {CameraSource::TestPattern, "test"}};

}  // namespace

const char* camera_name(CameraSource camera) {
  for (const auto& [c, name] : kCameras)
    if (c == camera) return name;
  return "off";
}

bool parse_camera(const std::string& name, CameraSource* camera) {
  for (const auto& [c, n] : kCameras) {
    if (name == n) {
      *camera = c;
      return true;
    }
  }
  return false;
}

const char* aspect_name(AspectRatio aspect) {
  for (const AspectInfo& a : kAspects)
    if (a.aspect == aspect) return a.name;
  return "auto";
}

bool parse_aspect(const std::string& name, AspectRatio* aspect) {
  for (const AspectInfo& a : kAspects) {
    if (name == a.name) {
      *aspect = a.aspect;
      return true;
    }
  }
  return false;
}

u32 widescreen_side(AspectRatio aspect, int window_width, int window_height) {
  double ratio = 4.0 / 3.0;
  for (const AspectInfo& a : kAspects)
    if (a.aspect == aspect) ratio = a.ratio;
  if (aspect == AspectRatio::Auto) {
    ratio = window_width > 0 && window_height > 0 ? static_cast<double>(window_width) / window_height : 4.0 / 3.0;
  }
  // The game's picture is 640x480 and 4:3, so its pixels are square: at
  // `ratio`, the picture is 480 * ratio pixels wide. Up to 32:9.
  ratio = std::min(ratio, 32.0 / 9.0);
  if (ratio <= 4.0 / 3.0 + 0.01) return 0;
  return static_cast<u32>(std::lround((480.0 * ratio - 640.0) / 2.0));
}

u32 internal_scale(u32 setting, int window_height) {
  if (setting) return std::clamp(setting, 1u, 8u);
  if (window_height <= 0) return 1;
  return std::clamp(static_cast<u32>((window_height + 479) / 480), 1u, 8u);
}

bool load_settings(const std::string& path, Settings* settings, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return true;  // defaults
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    const toml::source_position pos = parsed.error().source().begin;
    if (error) *error = path + ": line " + std::to_string(pos.line) + ": " + std::string(parsed.error().description());
    return false;
  }
  const toml::table& root = parsed.table();
  Settings s = *settings;
  const auto video = root["video"];
  s.fullscreen = video["fullscreen"].value_or(s.fullscreen);
  s.window_width = std::clamp(video["window_width"].value_or(s.window_width), 320, 16384);
  s.window_height = std::clamp(video["window_height"].value_or(s.window_height), 240, 16384);
  if (const auto aspect = video["aspect_ratio"].value<std::string>(); aspect && !parse_aspect(*aspect, &s.aspect)) {
    if (error) *error = path + ": unknown aspect_ratio \"" + *aspect + "\"";
    return false;
  }
  s.internal_resolution = static_cast<u32>(std::clamp(video["internal_resolution"].value_or(0), 0, 8));
  s.vsync = video["vsync"].value_or(s.vsync);
  s.volume = std::clamp(root["audio"]["volume"].value_or(s.volume), 0, 100);
  const auto game = root["game"];
  s.free_play = game["free_play"].value_or(s.free_play);
  s.pause_in_menu = game["pause_in_menu"].value_or(s.pause_in_menu);
  s.pause_in_background = game["pause_in_background"].value_or(s.pause_in_background);
  s.card = game["card"].value_or(s.card);
  const auto camera = root["camera"];
  if (const auto source = camera["source"].value<std::string>(); source && !parse_camera(*source, &s.camera)) {
    if (error) *error = path + ": unknown camera source \"" + *source + "\"";
    return false;
  }
  s.webcam = camera["webcam"].value_or(s.webcam);
  s.rich_presence = root["discord"]["rich_presence"].value_or(s.rich_presence);
  const auto link = root["link"];
  s.cabinet = std::clamp(link["cabinet"].value_or(s.cabinet), 0, 4);
  s.lan = link["lan"].value_or(s.lan);
  s.link_port = std::clamp(link["port"].value_or(s.link_port), 1024, 65535 - 40);
  *settings = std::move(s);
  return true;
}

bool save_settings(const std::string& path, const Settings& s, std::string* error) {
  std::ostringstream out;
  out << "# OpenGP settings. The menu (Esc) changes them too.\n"
         "\n"
         "[video]\n"
         "fullscreen = "
      << (s.fullscreen ? "true" : "false")
      << "\n"
         "# The window's size when not fullscreen.\n"
         "window_width = "
      << s.window_width << "\nwindow_height = " << s.window_height
      << "\n"
         "# \"auto\" (the window's shape), \"4:3\", \"16:9\", \"16:10\" or \"21:9\".\n"
         "aspect_ratio = \""
      << aspect_name(s.aspect)
      << "\"\n"
         "# 1 to 8 times the game's 640x528, or 0 to match the window's size.\n"
         "internal_resolution = "
      << s.internal_resolution << "\nvsync = " << (s.vsync ? "true" : "false")
      << "\n"
         "\n"
         "[audio]\n"
         "volume = "
      << s.volume
      << "  # percent\n"
         "\n"
         "[game]\n"
         "# Play without coins.\n"
         "free_play = "
      << (s.free_play ? "true" : "false")
      << "\n"
         "# Pause the game while the menu is open, and while its window is in the background.\n"
         "pause_in_menu = "
      << (s.pause_in_menu ? "true" : "false") << "\npause_in_background = " << (s.pause_in_background ? "true" : "false")
      << "\n"
         "# The Mario Card you hold (user/cards/<game>/<name>.bin), or \"\" for none.\n"
         "card = "
      << quoted(s.card)
      << "\n"
         "\n"
         "[camera]\n"
         "# The cabinet's camera, for the players' photos: \"off\", \"webcam\" or \"test\" (colour bars).\n"
         "source = \""
      << camera_name(s.camera)
      << "\"\n"
         "# The webcam's name, or \"\" for the first one.\n"
         "webcam = "
      << quoted(s.webcam)
      << "\n"
         "\n"
         "[discord]\n"
         "# Show what you are playing (character, cup, course, place) on your Discord profile, while\n"
         "# Discord runs on this computer.\n"
         "rich_presence = "
      << (s.rich_presence ? "true" : "false")
      << "\n"
         "\n"
         "[link]\n"
         "# Linked cabinets: give each OpenGP its own number, 1 to 4 (0 keeps the game's own setting).\n"
         "cabinet = "
      << s.cabinet
      << "\n"
         "# Also link with cabinets on other computers on the local network.\n"
         "lan = "
      << (s.lan ? "true" : "false")
      << "\n"
         "# The first of the 40 ports the link uses on this computer.\n"
         "port = "
      << s.link_port << "\n";
  const std::string text = out.str();
  if (!os::write_file_atomically(path, text.data(), text.size())) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace app
