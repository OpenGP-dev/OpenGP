// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame captures (the capture control, --capture-frame N, or the menu's
// Profiler tab): one
// displayed frame with everything needed to see why it looks wrong, in a
// folder (captures/<n>_f<frame>/ in the profile being recorded, else
// user/captures/<date>_<time>_f<frame>/):
//
//   capture.txt       every command in order: each draw's primitive, bounds and depth range on
//                     screen, blending and depth, TEV stages as formulas, textures (with their
//                     files), vertex format and lighting, and the game function that drew it;
//                     each EFB copy and clear. Made to be read.
//   capture.json      the same, for tools (tools/profiles.py capture ...)
//   screen.png        what was shown (the XFB, at the internal resolution)
//   efb_end.png       the EFB just before its copy to the XFB; depth_end.png its depth as grey
//                     (near is white), depth_end_raw.png the exact 24-bit depths (red, green,
//                     blue bytes, red highest)
//   efb_start.png     ... and the same as the frame found them (depth_start*.png)
//   textures/         every texture the frame decoded, t<handle>_<address>_<format>_<w>x<h>.png
//   copies/           every EFB copy's result, c<command>_<address>_<format>_<w>x<h>.png
//   earlier/          EFB copies of earlier frames the frame samples, as it found them
#pragma once

#include <filesystem>
#include <string>

#include "app/game_status.h"
#include "app/hang_watch.h"
#include "gx/frame.h"
#include "video/renderer.h"

namespace app {

struct CaptureInfo {
  std::string game;          // its title
  gekko::u64 number = 0;     // the displayed frame number, from power-on
  gekko::u32 scale = 1;      // the internal resolution
  GameStatus status;         // what the player was doing
  std::string settings;      // a line about the video settings
};

// Writes a captured frame (with its FrameCapture) into `dir`. Returns false
// with `error` set if the folder can't be written.
bool write_capture(const std::filesystem::path& dir, const gx::Frame& frame, const video::CaptureImages& images,
                   const FunctionNames& names, const CaptureInfo& info, std::string* error);

}  // namespace app
