// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

namespace audio {

namespace {

// Queue length (measured as each 5 ms buffer arrives) to hold, and the most
// to allow before dropping samples, in milliseconds.
constexpr double kTargetMs = 30;
constexpr double kMaxMs = 150;
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
  SDL_ResumeAudioStreamDevice(stream_);
  return true;
}

void HostOutput::set_volume(float volume) {
  if (stream_) SDL_SetAudioStreamGain(stream_, std::clamp(volume, 0.0f, 1.0f));
}

void HostOutput::push(const s16* samples, u32 frames, u32 rate) {
  if (!stream_ || !frames || !rate) return;
  if (rate != rate_) {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    rate_ = rate;
  }
  const double ms_per_byte = 1000.0 / (4.0 * rate);
  double queued_ms = SDL_GetAudioStreamQueued(stream_) * ms_per_byte;

  if (queued_ms < 1) {
    // Starting, or ran dry: rebuild the queue with silence.
    const std::vector<s16> silence(static_cast<size_t>(kTargetMs * rate / 1000) * 2, 0);
    SDL_PutAudioStreamData(stream_, silence.data(), static_cast<int>(silence.size() * sizeof(s16)));
    queued_ms = queued_avg_ms_ = kTargetMs;
    if (started_) underruns_.fetch_add(1);
    started_ = true;
  } else if (queued_ms > kMaxMs) {
    overflows_.fetch_add(1);
    return;  // far behind: drop
  }
  SDL_PutAudioStreamData(stream_, samples, static_cast<int>(frames * 4));

  // Drain faster when the queue is long, slower when it is short.
  queued_avg_ms_ += (queued_ms - queued_avg_ms_) * 0.05;
  queue_ms_.store(queued_avg_ms_);
  const double error = (queued_avg_ms_ - kTargetMs) / kTargetMs;
  const double ratio = 1.0 + std::clamp(error * 0.01, -kMaxRatioChange, kMaxRatioChange);
  SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio));
}

}  // namespace audio
