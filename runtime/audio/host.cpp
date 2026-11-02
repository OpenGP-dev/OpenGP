// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace audio {

namespace {

// Queue length (measured as each 5 ms buffer arrives) to hold at least, and
// at most; what each underrun adds; how far above the target the queue may
// stay before it is brought back; and the most to allow before dropping
// samples. In milliseconds.
constexpr double kTargetMs = 30;
constexpr double kMaxTargetMs = 80;
constexpr double kUnderrunStepMs = 10;
constexpr double kResyncMs = 60;
constexpr double kMaxMs = 200;
// How often to look at the device's buffer again (it changes with the
// device), in pushes of about 5 ms.
constexpr u32 kDeviceCheckPushes = 200;
// Largest speed-up or slow-down used to hold the target (0.5%: inaudible).
constexpr double kMaxRatioChange = 0.005;

}  // namespace

HostOutput::~HostOutput() {
  if (stream_) SDL_DestroyAudioStream(stream_);
}

bool HostOutput::init(float volume, std::string* error) {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    *error = SDL_GetError();
    return false;
  }
  rate_ = 32000;
  const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate_)};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!stream_) {
    *error = SDL_GetError();
    return false;
  }
  SDL_SetAudioStreamGain(stream_, std::clamp(volume, 0.0f, 1.0f));
  const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream_));
  device_name_ = name ? name : "default device";
  update_target();
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void HostOutput::update_target() {
  SDL_AudioSpec spec{};
  int frames = 0;
  if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &spec, &frames) && spec.freq > 0 && frames > 0)
    device_ms_ = 2000.0 * frames / spec.freq;
  target_ms_.store(std::min(std::max(kTargetMs, device_ms_) + underrun_ms_, kMaxTargetMs));
}

void HostOutput::set_volume(float volume) {
  if (stream_) SDL_SetAudioStreamGain(stream_, std::clamp(volume, 0.0f, 1.0f));
}

void HostOutput::set_paused(bool paused) {
  if (!stream_) return;
  if (paused) SDL_PauseAudioStreamDevice(stream_);
  else SDL_ResumeAudioStreamDevice(stream_);
}

void HostOutput::push(const s16* samples, u32 frames, u32 rate) {
  if (!stream_ || !frames || !rate) return;
  if (rate != rate_) {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    rate_ = rate;
  }
  if (++pushes_ >= kDeviceCheckPushes) {
    pushes_ = 0;
    update_target();
  }
  const double target = target_ms_.load();
  const double ms_per_byte = 1000.0 / (4.0 * rate);
  double queued_ms = SDL_GetAudioStreamQueued(stream_) * ms_per_byte;

  if (queued_ms < 1) {
    // Starting, or ran dry: rebuild the queue with silence, a little longer
    // after each time it runs dry.
    if (started_) {
      underruns_.fetch_add(1);
      underrun_ms_ = std::min(underrun_ms_ + kUnderrunStepMs, kMaxTargetMs);
      update_target();
    }
    started_ = true;
    resyncing_ = false;
    const double fill = target_ms_.load();
    const std::vector<s16> silence(static_cast<size_t>(fill * rate / 1000) * 2, 0);
    SDL_PutAudioStreamData(stream_, silence.data(), static_cast<int>(silence.size() * sizeof(s16)));
    queued_ms = queued_avg_ms_ = fill;
  } else if (queued_ms > kMaxMs) {
    overflows_.fetch_add(1);
    return;  // far behind: drop
  } else if (resyncing_ || queued_avg_ms_ > target + kResyncMs) {
    // Far too long for a while (the device stalled): drop until it is back
    // at the target, a short skip rather than the sound running late.
    if (!resyncing_) resyncs_.fetch_add(1);
    resyncing_ = queued_ms > target;
    if (resyncing_) {
      queued_avg_ms_ = queued_ms;
      queue_ms_.store(queued_ms);
      return;
    }
    queued_avg_ms_ = queued_ms;
  }
  SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * 4));

  // Drain faster when the queue is long, slower when it is short.
  queued_avg_ms_ += (queued_ms - queued_avg_ms_) * 0.05;
  queue_ms_.store(queued_avg_ms_);
  const double error = (queued_avg_ms_ - target) / target;
  const double ratio = 1.0 + std::clamp(error * 0.01, -kMaxRatioChange, kMaxRatioChange);
  SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio));
}

}  // namespace audio
