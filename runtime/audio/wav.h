// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes 16-bit stereo audio to a WAV file (for --audio-dump). The header is
// kept up to date after every write, so the file is valid even if the
// program exits without closing it.
#pragma once

#include <cstdio>
#include <string>

#include "gekko/types.h"

namespace audio {

using namespace gekko;

class WavWriter {
 public:
  WavWriter() = default;
  ~WavWriter() { close(); }
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  bool open(const std::string& path);
  void close();
  // Interleaved left/right samples. The first call sets the file's rate.
  void write(const s16* samples, u32 frames, u32 rate);

 private:
  void write_header();

  std::FILE* file_ = nullptr;
  u32 rate_ = 0;
  u32 frames_ = 0;
};

}  // namespace audio
