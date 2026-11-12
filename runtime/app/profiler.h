// SPDX-License-Identifier: GPL-3.0-or-later
// Live performance information. A bounded history in memory, with no recording,
// stack sampling, captures, worker threads or file output. Called by the render thread.
#pragma once

#include <string>
#include <vector>

#include "gekko/types.h"

namespace app {

class Profiler {
 public:
  struct FrameInfo {
    gekko::u64 finished_ns = 0, present_ns = 0, retrace = 0;
    gekko::u64 idle_ns = 0, wait_ns = 0;  // cumulative counters
    gekko::u64 render_ns = 0;
    bool active = true;
    double audio_ms = 0;
    gekko::u64 audio_underruns = 0, texture_bytes = 0;
    gekko::u32 textures = 0, pipelines = 0;
    std::string scene, course;
  };
  struct LiveStats {
    gekko::u64 hitches = 0, drops = 0;
    float fps = 0, guest_ms = 0, busy_ms = 0, present_ms = 0, p99_ms = 0, render_ms = 0;
    float system_cpu = -1;  // whole computer, percent; negative when unavailable
    float audio_ms = 0;
    gekko::u64 audio_underruns = 0;
    gekko::u32 textures = 0, pipelines = 0;
    double texture_mb = 0;
    std::string scene, course;
    std::vector<float> graph;  // at most 120 active frames, oldest first
  };

  void on_frame(const FrameInfo& info);
  // Poll at most once per second. Reads operating-system CPU counters only.
  void sample_system_cpu();
  const LiveStats& live() const { return live_; }

 private:
  LiveStats live_;
  FrameInfo last_;
  bool have_last_ = false, have_cpu_ = false;
  double present_average_ns_ = 0;
  gekko::u64 cpu_total_ = 0, cpu_idle_ = 0;
};

}  // namespace app
