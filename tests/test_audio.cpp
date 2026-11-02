// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sound output's queue (runtime/audio/host.h), on SDL's dummy audio
// driver, which plays in real time without a sound card.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "audio/host.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Pushes 5 ms bursts in real time for `duration`, as the game does.
void play(audio::HostOutput& out, std::chrono::milliseconds duration) {
  constexpr gekko::u32 kRate = 32000, kFrames = kRate / 200;
  const std::vector<gekko::s16> burst(kFrames * 2, 0);
  const auto end = Clock::now() + duration;
  auto next = Clock::now();
  while (Clock::now() < end) {
    out.push(burst.data(), kFrames, kRate);
    next += 5ms;
    std::this_thread::sleep_until(next);
  }
}

bool open(audio::HostOutput& out) {
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  std::string error;
  if (!out.init(0.0f, &error)) {
    MESSAGE("no dummy audio driver: " << error);
    return false;
  }
  return true;
}

}  // namespace

TEST_CASE("after the device stalls, the sound catches up at once instead of running late") {
  audio::HostOutput out;
  if (!open(out)) return;
  play(out, 500ms);
  const double target = out.target_ms();
  CHECK(target >= 30.0);
  CHECK(out.queue_ms() < target + 30.0);

  // The device stops taking samples for a while (as while the system
  // switches to another device): the queue fills up.
  out.set_paused(true);
  play(out, 400ms);
  out.set_paused(false);
  // Within a second it is back near the target (the old controller, at 0.5%
  // faster, took about 20 seconds).
  play(out, 1000ms);
  CHECK(out.resyncs() >= 1);
  CHECK(out.queue_ms() < target + 30.0);
}
