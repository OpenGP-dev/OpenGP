// SPDX-License-Identifier: GPL-3.0-or-later
//
// A webcam's first frames after it is switched on are dark: its automatic
// exposure takes a moment (a fraction of a second to a second or two) to
// settle. Warmup says when the frames can be shown: once their brightness has
// stayed steady for a few frames, or after kMaxWait whatever it does. Until
// then the camera sends a grey picture, as before the first frame, so a dark
// warm-up frame never becomes a player's photo.
#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>

#include "gekko/types.h"

namespace camera {

class Warmup {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kMinWait = std::chrono::milliseconds(250);
  static constexpr auto kMaxWait = std::chrono::seconds(2);
  static constexpr int kSteadyFrames = 3;     // in a row
  static constexpr double kSteadyChange = 3;  // most a frame's mean brightness (0-255) may change

  // The webcam has just been switched on.
  void start(Clock::time_point now) {
    started_ = now;
    settled_ = false;
    steady_ = 0;
    last_mean_ = -1.0;
  }

  // A frame came, with this mean brightness (0-255). True if it may be shown
  // (and every one after it, until start() again).
  bool frame(double mean, Clock::time_point now) {
    if (settled_) return true;
    const auto age = now - started_;
    if (last_mean_ >= 0.0 && std::fabs(mean - last_mean_) <= kSteadyChange) ++steady_;
    else steady_ = 0;
    last_mean_ = mean;
    settled_ = (age >= kMinWait && steady_ >= kSteadyFrames) || age >= kMaxWait;
    return settled_;
  }

  // The mean brightness of an RGB24 picture (a sample of its pixels).
  static double mean_brightness(const gekko::u8* rgb, std::size_t pixels) {
    if (pixels == 0) return 0.0;
    const std::size_t step = pixels > 4096 ? pixels / 4096 : 1;
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < pixels; i += step, ++n) {
      const gekko::u8* p = rgb + i * 3;
      sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
    }
    return sum / static_cast<double>(n);
  }

 private:
  Clock::time_point started_{};
  bool settled_ = false;
  int steady_ = 0;
  double last_mean_ = -1.0;
};

}  // namespace camera
