// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read access to a GameCube-format disc image (.iso / .gcm). Triforce games
// ship as such images; the media board serves reads from them.
#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

  // Opens an image (`path` in UTF-8) and checks the GameCube magic word.
  bool open(const std::string& path, std::string* error);
  bool is_open() const { return file_ != nullptr; }

  // Reads `size` bytes at `offset`. Thread-safe. Bytes past the end of the
  // image read as zero (and the call returns false).
  bool read(u64 offset, void* dst, u64 size) const;
  u32 read_u32(u64 offset) const;  // big-endian

  // Reads the whole image into memory on a background thread, as a Triforce
  // media board holds it in its DIMM memory. Reads of the part loaded so far
  // come from memory, the rest from the file. The games read the disc while
  // they run (GP 2 streams its music during races), and a read the system's
  // file cache doesn't have takes milliseconds, once more than a second.
  void preload();
  u64 preloaded() const { return loaded_.load(std::memory_order_acquire); }  // bytes in memory so far

  // The executable (sys/main.dol): its offset is in the disc header, and its
  // size follows from its section table.
  bool read_dol(std::vector<u8>* out) const;

  // A file on the disc, by its path in the disc's file system ("mnTitle.tpl",
  // "sub/name.dat"; case-sensitive). Returns false if there is no such file.
  bool find_file(const std::string& path, u64* offset, u32* size) const;
  bool read_file(const std::string& path, std::vector<u8>* out) const;

  // Treats the image as having this game ID: reads of the header's first six
  // bytes return it. For images whose header has a placeholder ID (some
  // converted dumps say "RELSAB") around the right executable.
  void set_game_id(const std::string& id);

  u64 size() const { return size_; }
  const std::string& game_id() const { return game_id_; }  // e.g. "GGPE02"
  const std::string& path() const { return path_; }

 private:
  std::FILE* file_ = nullptr;
  u64 size_ = 0;
  std::string game_id_;
  bool id_override_ = false;
  std::string path_;
  mutable std::mutex mutex_;

  std::unique_ptr<u8[]> memory_;   // the image, filled from the start by the preload thread
  std::atomic<u64> loaded_{0};     // bytes of memory_ filled so far
  std::atomic<bool> stop_{false};  // asks the preload thread to stop
  std::thread preload_thread_;
};

// The SHA-1 of the image's main.dol, in hex ("" if it cannot be read).
std::string dol_sha1(const Image& img);

}  // namespace disc
