// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/wav.h"

namespace audio {

namespace {

void put16(u8* p, u32 v) {
  p[0] = static_cast<u8>(v);
  p[1] = static_cast<u8>(v >> 8);
}

void put32(u8* p, u32 v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}

}  // namespace

bool WavWriter::open(const std::string& path) {
  close();
  file_ = std::fopen(path.c_str(), "wb");
  rate_ = 0;
  frames_ = 0;
  if (file_) write_header();
  return file_ != nullptr;
}

void WavWriter::close() {
  if (!file_) return;
  write_header();
  std::fclose(file_);
  file_ = nullptr;
}

void WavWriter::write(const s16* samples, u32 frames, u32 rate) {
  if (!file_) return;
  if (!rate_) rate_ = rate;
  // Little-endian on disk.
  u8 buf[4096];
  u32 done = 0;
  while (done < frames * 2) {
    u32 n = 0;
    for (; n < sizeof(buf) / 2 && done < frames * 2; ++n, ++done) put16(buf + n * 2, static_cast<u16>(samples[done]));
    std::fwrite(buf, 2, n, file_);
  }
  frames_ += frames;
  write_header();
}

void WavWriter::write_header() {
  const u32 data_bytes = frames_ * 4;
  u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  put32(h + 4, 36 + data_bytes);
  put32(h + 16, 16);  // fmt chunk size
  put16(h + 20, 1);   // PCM
  put16(h + 22, 2);   // channels
  put32(h + 24, rate_ ? rate_ : 32000);
  put32(h + 28, (rate_ ? rate_ : 32000) * 4);
  put16(h + 32, 4);   // block align
  put16(h + 34, 16);  // bits per sample
  h[36] = 'd', h[37] = 'a', h[38] = 't', h[39] = 'a';
  put32(h + 40, data_bytes);
  const long end = std::ftell(file_);
  std::fseek(file_, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), file_);
  std::fseek(file_, end > 44 ? end : 44, SEEK_SET);
  std::fflush(file_);
}

}  // namespace audio
