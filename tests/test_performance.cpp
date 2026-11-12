// SPDX-License-Identifier: GPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "app/profiler.h"

using app::Profiler;

TEST_CASE("live timings exclude idle and waits and retain only 120 frames") {
  Profiler p;
  Profiler::FrameInfo f;
  f.active = true;
  for (int i = 0; i < 300; ++i) {
    f.finished_ns += 16'000'000;
    f.present_ns += 20'000'000;
    f.idle_ns += 3'000'000;
    f.wait_ns += 2'000'000;
    ++f.retrace;
    p.on_frame(f);
  }
  const auto& live = p.live();
  CHECK(live.fps == doctest::Approx(50));
  CHECK(live.guest_ms == doctest::Approx(16));
  CHECK(live.busy_ms == doctest::Approx(11));
  CHECK(live.p99_ms == doctest::Approx(16));
  CHECK(live.graph.size() == 120);
  CHECK(live.hitches == 0);
  CHECK(live.drops == 0);
}

TEST_CASE("first frame, pause and resume do not create hitches or dropped frames") {
  Profiler p;
  Profiler::FrameInfo f;
  f.finished_ns = f.present_ns = 1'000'000'000;
  f.retrace = 60;
  p.on_frame(f);
  CHECK(p.live().graph.empty());
  f.active = false;
  f.finished_ns = f.present_ns = 10'000'000'000;
  f.retrace = 600;
  p.on_frame(f);
  f.active = true;
  f.finished_ns = f.present_ns = 20'000'000'000;
  f.retrace = 1200;
  p.on_frame(f);
  CHECK(p.live().hitches == 0);
  CHECK(p.live().drops == 0);
  f.finished_ns += 50'000'000;
  f.present_ns += 50'000'000;
  f.retrace += 3;
  p.on_frame(f);
  CHECK(p.live().hitches == 1);
  CHECK(p.live().drops == 2);
}

TEST_CASE("counter resets and overlapping wait totals cannot underflow busy time") {
  Profiler p;
  Profiler::FrameInfo f;
  f.finished_ns = f.present_ns = 100'000'000;
  f.idle_ns = f.wait_ns = 50'000'000;
  p.on_frame(f);
  f.finished_ns += 16'000'000;
  f.present_ns += 16'000'000;
  f.idle_ns = f.wait_ns = 0;
  p.on_frame(f);
  CHECK(p.live().busy_ms == doctest::Approx(16));
  f.finished_ns += 16'000'000;
  f.present_ns += 16'000'000;
  f.idle_ns = f.wait_ns = 25'000'000;
  p.on_frame(f);
  CHECK(p.live().busy_ms == 0);
}

TEST_CASE("resource and scene information updates even while paused") {
  Profiler p;
  Profiler::FrameInfo f;
  f.active = false;
  f.textures = 10;
  f.pipelines = 4;
  f.texture_bytes = 2 * 1024 * 1024;
  f.audio_ms = 32;
  f.audio_underruns = 2;
  f.scene = "Racing";
  f.course = "Test course";
  p.on_frame(f);
  CHECK(p.live().textures == 10);
  CHECK(p.live().pipelines == 4);
  CHECK(p.live().texture_mb == 2);
  CHECK(p.live().audio_ms == 32);
  CHECK(p.live().audio_underruns == 2);
  CHECK(p.live().scene == "Racing");
  CHECK(p.live().course == "Test course");
}
