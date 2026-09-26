// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read access to a GameCube-format disc image (.iso / .gcm). Triforce games
// ship as such images; the media board serves reads from them.
#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include "gekko/types.h"

namespace disc {

using namespace gekko;

inline constexpr u32 GC_MAGIC = 0xC2339F3D;  // at offset 0x1C of the disc header

class Image {
 public:
  Image() = default;
  ~Image();
  Image(const Image&) = delete;
  Image& operator=(const Image&) = delete;

  // Opens an image and checks the GameCube magic word.
  bool open(const std::string& path, std::string* error);
  bool is_open() const { return file_ != nullptr; }

  // Reads `size` bytes at `offset`. Thread-safe. Bytes past the end of the
  // image read as zero (and the call returns false).
  bool read(u64 offset, void* dst, u64 size) const;
  u32 read_u32(u64 offset) const;  // big-endian

  u64 size() const { return size_; }
  const std::string& game_id() const { return game_id_; }  // e.g. "GGPE02"
  const std::string& path() const { return path_; }

 private:
  std::FILE* file_ = nullptr;
  u64 size_ = 0;
  std::string game_id_;
  std::string path_;
  mutable std::mutex mutex_;
};

}  // namespace disc
