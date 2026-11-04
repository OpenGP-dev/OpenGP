// SPDX-License-Identifier: GPL-3.0-or-later
//
// A webcam's brightness, for the player's photo.
//
// Warmup: a webcam's first frames after it is switched on are dark or
// flickering while its automatic exposure settles (a fraction of a second to
// a second or two). Warmup says when the frames can be shown: once their
// brightness has stayed steady for a few frames, or after kMaxWait whatever
// it does, counted from the first frame (opening a webcam can itself take a
// while). Until then the camera sends a grey picture, as before the first
// frame, so a warm-up frame never becomes a player's photo.
//
// AutoGain: the cabinet's camera had its own light; a webcam in a dim room
// gives an almost black picture. AutoGain brightens a dark picture towards a
// normal brightness, at most kMaxGain times, changing gradually from frame to
// frame so that the picture does not flicker. A picture that is bright enough
// is left as it is.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

#include "gekko/types.h"

namespace camera {

// The mean brightness (0-255) of an RGB24 picture, from a sample of its pixels.
inline double mean_brightness(const gekko::u8* rgb, std::size_t pixels) {
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

class Warmup {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kMinWait = std::chrono::milliseconds(250);
  static constexpr auto kMaxWait = std::chrono::seconds(2);
  static constexpr int kSteadyFrames = 3;     // in a row
  static constexpr double kSteadyChange = 3;  // most a frame's mean brightness (0-255) may change

  // The webcam has just been switched on.
  void start() {
    first_ = true;
    settled_ = false;
    steady_ = 0;
    last_mean_ = -1.0;
  }

  // A frame came, with this mean brightness (0-255). True if it may be shown
  // (and every one after it, until start() again).
  bool frame(double mean, Clock::time_point now) {
    if (settled_) return true;
    if (first_) {
      first_ = false;
      first_frame_ = now;
    }
    const auto age = now - first_frame_;
    if (last_mean_ >= 0.0 && std::fabs(mean - last_mean_) <= kSteadyChange) ++steady_;
    else steady_ = 0;
    last_mean_ = mean;
    settled_ = (age >= kMinWait && steady_ >= kSteadyFrames) || age >= kMaxWait;
    return settled_;
  }

 private:
  Clock::time_point first_frame_{};
  bool first_ = true;
  bool settled_ = false;
  int steady_ = 0;
  double last_mean_ = -1.0;
};

class AutoGain {
 public:
  static constexpr double kTarget = 100;   // the mean brightness (0-255) to bring a dark picture up to
  static constexpr double kMaxGain = 6;
  static constexpr double kSmoothing = 0.15;  // how much of the way to the new gain each frame goes

  void reset() { gain_ = -1.0; }

  // Brightens an RGB24 picture of `pixels` pixels in place, given its mean
  // brightness. Returns the gain used.
  double apply(gekko::u8* rgb, std::size_t pixels, double mean) {
    const double wanted = std::clamp(kTarget / std::max(mean, 1.0), 1.0, kMaxGain);
    gain_ = gain_ < 0.0 ? wanted : gain_ + (wanted - gain_) * kSmoothing;
    if (gain_ <= 1.01) return 1.0;
    const auto g = static_cast<int>(std::lround(gain_ * 256.0));
    for (std::size_t i = 0; i < pixels * 3; ++i) rgb[i] = static_cast<gekko::u8>(std::min(rgb[i] * g >> 8, 255));
    return gain_;
  }

 private:
  double gain_ = -1.0;
};

}  // namespace camera
