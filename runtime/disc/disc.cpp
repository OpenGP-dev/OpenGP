// SPDX-License-Identifier: GPL-3.0-or-later

#include "disc/disc.h"

#include <cstring>

namespace disc {

namespace {

int seek64(std::FILE* f, u64 offset) {
#ifdef _WIN32
  return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
  return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

u64 tell64(std::FILE* f) {
#ifdef _WIN32
  return static_cast<u64>(_ftelli64(f));
#else
  return static_cast<u64>(ftello(f));
#endif
}

}  // namespace

Image::~Image() {
  if (file_) std::fclose(file_);
}

bool Image::open(const std::string& path, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = path + ": " + msg;
    if (file_) std::fclose(file_);
    file_ = nullptr;
    return false;
  };
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) return fail("cannot open");
  if (std::fseek(file_, 0, SEEK_END) != 0) return fail("cannot seek");
  size_ = tell64(file_);
  path_ = path;
  u8 header[0x20];
  if (!read(0, header, sizeof(header))) return fail("too small for a disc header");
  const u32 magic = (u32{header[0x1C]} << 24) | (u32{header[0x1D]} << 16) | (u32{header[0x1E]} << 8) | header[0x1F];
  if (magic != GC_MAGIC) return fail("not a GameCube/Triforce disc image");
  game_id_.assign(reinterpret_cast<const char*>(header), 6);
  return true;
}

bool Image::read(u64 offset, void* dst, u64 size) const {
  std::memset(dst, 0, size);
  if (!file_ || offset >= size_) return false;
  const u64 avail = size_ - offset;
  const u64 n = size < avail ? size : avail;
  std::lock_guard lock(mutex_);
  if (seek64(file_, offset) != 0) return false;
  const bool ok = std::fread(dst, 1, n, file_) == n;
  return ok && n == size;
}

u32 Image::read_u32(u64 offset) const {
  u8 b[4];
  read(offset, b, 4);
  return (u32{b[0]} << 24) | (u32{b[1]} << 16) | (u32{b[2]} << 8) | b[3];
}

}  // namespace disc
