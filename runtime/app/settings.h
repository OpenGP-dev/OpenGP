// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's settings (user/settings.toml): video, sound and game options.
// The launcher reads them at start-up, command-line options override them for
// one run, and the in-game menu changes and saves them.
#pragma once

#include <string>

#include "gekko/types.h"

namespace app {

using namespace gekko;

enum class AspectRatio : u8 {
  Auto,      // the window's shape (4:3 or wider)
  Original,  // 4:3
  Wide16x9,
  Wide16x10,
  Wide21x9,
};

// Where the cabinet's camera (the NamCam2) gets its pictures.
enum class CameraSource : u8 {
  Off,          // no camera: the game takes no photos
  Webcam,       // a webcam
  TestPattern,  // colour bars
};

struct Settings {
  // [video]
  bool fullscreen = false;
  int window_width = 1280, window_height = 720;  // the window's size when not fullscreen
  AspectRatio aspect = AspectRatio::Auto;
  u32 internal_resolution = 0;  // 0: from the window's size, else 1 to 8 times 640x528
  bool vsync = true;

  // [audio]
  int volume = 100;  // percent

  // [game]
  bool free_play = false;
  bool attract_sound_every_loop = false;  // otherwise the first pass is quiet, as on a cabinet
  bool pause_in_menu = true;         // the game pauses while the menu is open
  bool pause_in_background = false;  // ... and while its window is not the active one
  std::string card = "Card 1";  // the Mario Card the player holds (a file in user/cards/); "" for none

  // [camera] (read at start-up: changes take effect when the game restarts)
  CameraSource camera = CameraSource::Off;
  std::string webcam;  // the webcam's name; "" for the first

  bool operator==(const Settings&) const = default;
};

// Reads `path` over `settings` (what the file leaves out keeps its value).
// Returns false with `error` set if the file cannot be parsed; a missing file
// is not an error.
bool load_settings(const std::string& path, Settings* settings, std::string* error);

// Writes `settings` to `path` (replacing it only once written in full).
bool save_settings(const std::string& path, const Settings& settings, std::string* error);

// Camera source names as used in the file and on the command line ("off",
// "webcam", "test").
const char* camera_name(CameraSource camera);
bool parse_camera(const std::string& name, CameraSource* camera);

// Aspect ratio names as used in the file and on the command line ("auto",
// "4:3", "16:9", "16:10", "21:9").
const char* aspect_name(AspectRatio aspect);
bool parse_aspect(const std::string& name, AspectRatio* aspect);

// The pixels to add on each side of the game's 640-pixel-wide picture for an
// aspect ratio (see gx::Frontend::set_widescreen), given the window's size
// in pixels for AspectRatio::Auto (0x0: none, so 4:3).
u32 widescreen_side(AspectRatio aspect, int window_width, int window_height);

// The internal resolution for a setting: `setting` itself, or for 0 the
// smallest scale whose picture is at least as tall as the window.
u32 internal_scale(u32 setting, int window_height);

}  // namespace app
