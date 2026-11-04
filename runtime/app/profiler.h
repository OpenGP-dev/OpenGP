// SPDX-License-Identifier: GPL-3.0-or-later
//
// Profiling while playing: --profile, the profiler's controls (F10 records
// and stops by default), or the menu's Profiler tab, record everything about
// the session into a profile (see profile/session.h for its files, and
// tools/profiles.py to read it): every frame's timings and counts, the game's
// scenes and races, hitches and what caused them, likely graphical glitches,
// shader compiles, CPU use, call stack samples, thumbnails of the screen, and
// the moments the player marks (F8 by default).
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "app/game_status.h"
#include "app/settings.h"
#include "gekko/types.h"
#include "gx/frame.h"
#include "profile/monitor.h"
#include "profile/session.h"

namespace opengp {
class Machine;
}
namespace video {
class Renderer;
struct RenderStats;
}

namespace app {

struct Game;

class Profiler {
 public:
  struct Options {
    int rate_hz = 500;           // call stack samples a second per busy thread (0: none)
    int thumbnail_every = 120;   // frames between thumbnails (0: none, except for events)
    gekko::u32 thumbnail_width = 480;
  };

  // What the launcher knows about a frame it has just presented.
  struct FrameInfo {
    gekko::u64 number = 0;  // displayed frame number, from power-on
    bool paused = false, menu_open = false, background = false, linked = false;
    const Settings* settings = nullptr;
    gekko::u32 scale = 1;          // the internal resolution in use
    gekko::u64 queue_ns = 0;       // the main thread's wait for the frame
    gekko::u32 queue_depth = 0;    // frames waiting after it
    gekko::u64 pace_ns = 0;        // ... holding it until its retrace (even frame pacing)
    double audio_queue_ms = 0;
    gekko::u64 audio_underruns = 0;  // so far
  };

  // The live numbers the overlay and the Profiler page show: taken after
  // every presented frame, whether recording or not.
  struct LiveStats {
    bool recording = false, auto_capture = false;
    gekko::u64 frames = 0, marks = 0;  // this recording's (0 when not recording)
    gekko::u64 hitches = 0, drops = 0;  // this recording's, or live (see update_live)
    double seconds = 0;
    float fps = 0, guest_ms = 0, busy_ms = 0, present_ms = 0, p99_ms = 0, render_ms = 0;
    float system_cpu = -1;  // percent of the whole computer, -1 if unknown
    float audio_ms = 0;
    gekko::u64 audio_underruns = 0;
    gekko::u32 textures = 0, pipelines = 0;
    double texture_mb = 0;
    std::string scene, course;
    std::vector<float> graph;  // the last frames' game times, ms, oldest first
  };
  LiveStats live() const;

  // A frame capture when a hitch happens, or a likely glitch is first seen,
  // while recording (at most one every few seconds). Off by default.
  void set_auto_capture(bool on);
  bool auto_capture() const;
  // Whether a capture was asked for (and clears it): the launcher captures
  // the next frame.
  bool take_capture_request();

  Profiler(opengp::Machine& machine, const Game& game, video::Renderer& renderer, Options options);
  ~Profiler();

  // Describes the session for session.json: the host (display, window,
  // command line...), merged into what the profiler knows itself.
  void set_describe(std::function<profile::Json()> describe) { describe_ = std::move(describe); }

  // Starts recording into `dir` (empty: a new folder in `profiles_dir`), or
  // goes on with a recording a restart interrupted (`resume`).
  bool start(std::filesystem::path dir, const std::filesystem::path& profiles_dir, bool resume, std::string* error);
  // Stops recording (thread-safe).
  void stop(const char* why);
  bool recording() const { return session_.is_open(); }
  std::filesystem::path dir() const { return session_.dir(); }
  gekko::u64 frames() const { return session_.frames_written(); }

  // After each presented frame (the renderer's statistics are taken even
  // when not recording).
  void on_frame(const gx::Frame& frame, const FrameInfo& info);
  // Marks the moment (the mark control): an event, a thumbnail and a flag on
  // the frame.
  void mark(const std::string& note);
  // Notes something the launcher saw (a screenshot, a hang report...).
  void note(std::string_view type, const profile::Json& fields = {});
  // Writes what is buffered, unless a file is being written (after a crash).
  void flush() { session_.flush(false); }

 private:
  struct SceneStats {
    std::string kind;
    gekko::u64 frames = 0, guest_us = 0, hitches = 0;
    std::vector<gekko::u32> times;  // guest_us of each frame
  };
  struct RiskStats {
    gekko::u64 count = 0, first_frame = 0;
    double first_t = 0;
  };

  void scene_events(const GameStatus& status);
  void settings_events(const FrameInfo& info);
  void update_live(const gx::Frame& frame, const FrameInfo& info, const video::RenderStats& rs, gekko::u64 retrace);
  void maybe_auto_capture();
  void hitch(const gx::Frame& frame, const profile::FrameRecord& r, const video::RenderStats& rs, gekko::u32 expected_us);
  void write_thumbnails();
  profile::Json summary(const char* why) const;

  opengp::Machine& machine_;
  const Game& game_;
  video::Renderer& renderer_;
  Options options_;
  std::function<profile::Json()> describe_;

  mutable std::mutex mutex_;
  profile::Session session_;
  profile::Monitor monitor_;

  // Since the start of this run's recording.
  gekko::u64 frames_ = 0, hitches_ = 0, marks_ = 0, drops_ = 0, pipeline_binds_ = 0, max_texture_kb_ = 0;
  gekko::u64 last_exceptions_ = 0, last_idle_ns_ = 0, last_underruns_ = 0;
  gekko::u64 last_present_ns_ = 0, last_retrace_ = 0;
  bool have_retrace_ = false;
  gekko::u64 next_thumbnail_ = 0;
  bool thumbnail_now_ = false;
  bool mark_pending_ = false;
  gekko::u32 frames_since_scene_ = 0, frames_since_pause_ = 0;
  std::vector<gekko::u32> recent_;  // the last frames' guest_us (not paused), for the expected frame time
  std::size_t recent_next_ = 0;
  std::vector<gekko::u32> guest_times_, busy_times_, present_times_;
  std::map<std::string, gekko::u64> hitch_causes_;
  std::map<gekko::u32, SceneStats> scenes_;
  std::map<std::tuple<int, gekko::u32, gekko::u32, gekko::u32>, RiskStats> risks_;
  gekko::u64 pipelines_ = 0;
  GameStatus last_status_;
  bool have_status_ = false;
  gekko::u32 last_flags_ = 0;
  Settings last_settings_;
  gekko::u32 last_scale_ = 0, last_efb_width_ = 0;
  bool have_settings_ = false;

  // The live numbers (see live()), and auto captures.
  LiveStats live_;
  std::vector<float> live_graph_;
  gekko::u64 live_hitches_ = 0, last_live_present_ns_ = 0, last_live_retrace_ = 0, last_live_idle_ns_ = 0;
  double live_present_avg_us_ = 0;
  bool have_live_retrace_ = false;
  bool auto_capture_ = false, capture_request_ = false;
  gekko::u64 auto_captures_ = 0, last_auto_capture_us_ = 0;
};

}  // namespace app
