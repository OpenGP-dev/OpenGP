// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plays the game's audio on the default output device (through SDL3).
//
// The game produces samples in real time (the audio DMA runs on the time
// base, which follows the host clock), but the sound card has its own clock,
// and the samples arrive in 5 ms bursts. A short queue absorbs the bursts; its
// length is held near a target by resampling slightly faster or slower, and
// it is refilled with silence if it ever runs dry. The target suits the
// device (twice its buffer, at least 30 ms) and grows a little each time the
// queue runs dry, up to 80 ms. When the queue is far too long (the device
// stalled, as while the system switches to another one), new samples are
// dropped until it is back at the target, rather than playing late.
#pragma once

#include <atomic>
#include <string>

#include "gekko/types.h"

struct SDL_AudioStream;

namespace audio {

using namespace gekko;

class HostOutput {
 public:
  HostOutput() = default;
  ~HostOutput();
  HostOutput(const HostOutput&) = delete;
  HostOutput& operator=(const HostOutput&) = delete;

  // Opens the default playback device. `volume` is 0 to 1.
  bool init(float volume, std::string* error);
  const char* device_name() const { return device_name_.c_str(); }

  // 0 to 1. Thread-safe.
  void set_volume(float volume);

  // Stops playing, keeping what is queued for when it resumes (while the
  // game is paused). Thread-safe.
  void set_paused(bool paused);

  // Interleaved left/right samples at `rate` Hz. Thread-safe.
  void push(const s16* samples, u32 frames, u32 rate);

  // How often the queue ran dry (a gap in the sound) or overflowed (samples
  // dropped), and its length in milliseconds.
  u64 underruns() const { return underruns_.load(); }
  u64 overflows() const { return overflows_.load(); }
  u64 resyncs() const { return resyncs_.load(); }
  double queue_ms() const { return queue_ms_.load(); }
  double target_ms() const { return target_ms_.load(); }

 private:
  SDL_AudioStream* stream_ = nullptr;
  std::string device_name_;
  u32 rate_ = 0;
  double queued_avg_ms_ = 0;
  double device_ms_ = 0;     // the queue the device itself needs: twice its buffer
  double underrun_ms_ = 0;   // added to the target by underruns
  u32 pushes_ = 0;           // since the device's buffer was last looked at
  bool started_ = false;
  bool resyncing_ = false;   // dropping samples until the queue is back at the target
  std::atomic<u64> underruns_{0}, overflows_{0}, resyncs_{0};
  std::atomic<double> queue_ms_{0}, target_ms_{30};
  void update_target();
};

}  // namespace audio
