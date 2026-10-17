// SPDX-License-Identifier: GPL-3.0-or-later

#include "disc/disc.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>

#include "disc/sha1.h"

namespace disc {

namespace {

int seek64(std::FILE* f, u64 offset) {
#ifdef _WIN32
  return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
  return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::FILE* open_utf8(const std::string& path) {
#ifdef _WIN32
  // fopen would read the name in the system's code page.
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 0) return nullptr;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
  return _wfopen(wide.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
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
  stop_ = true;
  if (preload_thread_.joinable()) preload_thread_.join();
  if (file_) std::fclose(file_);
}

void Image::preload() {
  if (!file_ || memory_) return;
  memory_.reset(new (std::nothrow) u8[size_]);
  if (!memory_) return;  // not enough memory: everything stays on the file
  preload_thread_ = std::thread([this] {
#ifdef _WIN32
    // Low CPU and I/O priority: the game's own reads go first.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
    std::FILE* f = open_utf8(path_);
    if (!f) return;
    constexpr u64 kChunk = 4 << 20;
    u64 pos = 0;
    while (pos < size_ && !stop_) {
      const u64 n = std::min(kChunk, size_ - pos);
      if (std::fread(memory_.get() + pos, 1, n, f) != n) break;  // the rest stays on the file
      pos += n;
      loaded_.store(pos, std::memory_order_release);
    }
    std::fclose(f);
  });
}

bool Image::open(const std::string& path, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = path + ": " + msg;
    if (file_) std::fclose(file_);
    file_ = nullptr;
    return false;
  };
  file_ = open_utf8(path);
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
  bool ok = true;
  if (offset + n <= loaded_.load(std::memory_order_acquire)) {
    std::memcpy(dst, memory_.get() + offset, n);
  } else {
    std::lock_guard lock(mutex_);
    ok = seek64(file_, offset) == 0 && std::fread(dst, 1, n, file_) == n;
  }
  if (id_override_)
    for (u64 i = offset; i < game_id_.size() && i < offset + n; ++i)
      static_cast<u8*>(dst)[i - offset] = static_cast<u8>(game_id_[i]);
  return ok && n == size;
}

void Image::set_game_id(const std::string& id) {
  game_id_ = id;
  id_override_ = true;
}

u32 Image::read_u32(u64 offset) const {
  u8 b[4];
  read(offset, b, 4);
  return (u32{b[0]} << 24) | (u32{b[1]} << 16) | (u32{b[2]} << 8) | b[3];
}

bool Image::read_dol(std::vector<u8>* out) const {
  constexpr u64 kDolOffset = 0x420;  // in the disc header
  constexpr u32 kHeaderSize = 0x100, kSections = 18;
  const u64 dol = read_u32(kDolOffset);
  if (dol == 0 || dol >= size_) return false;
  u32 end = kHeaderSize;
  for (u32 i = 0; i < kSections; ++i) {
    const u32 offset = read_u32(dol + 4 * i), size = read_u32(dol + 0x90 + 4 * i);
    if (size && offset + size > end) end = offset + size;
  }
  out->resize(end);
  return read(dol, out->data(), end);
}

bool Image::find_file(const std::string& path, u64* offset, u32* size) const {
  // The file system table (FST): its offset and size are in the disc header.
  // 12-byte entries, the root directory first, then the names. An entry is a
  // flags byte (1: directory), its name's offset (24 bits), then for a file
  // its offset and size, and for a directory its parent and the index past
  // its last entry.
  const u64 fst = read_u32(0x424);
  const u32 fst_size = read_u32(0x428);
  if (fst == 0 || fst_size < 12 || fst + fst_size > size_) return false;
  std::vector<u8> table(fst_size);
  if (!read(fst, table.data(), fst_size)) return false;
  auto be32 = [&](u32 at) {
    return (u32{table[at]} << 24) | (u32{table[at + 1]} << 16) | (u32{table[at + 2]} << 8) | table[at + 3];
  };
  const u32 count = be32(8);
  if (count == 0 || u64{count} * 12 > fst_size) return false;
  const u32 names = count * 12;
  auto name = [&](u32 entry) -> std::string_view {
    const u32 at = names + (be32(entry * 12) & 0xFFFFFF);
    if (at >= fst_size) return {};
    const char* s = reinterpret_cast<const char*>(table.data() + at);
    return std::string_view(s, strnlen(s, fst_size - at));
  };

  // Walk the path one name at a time, within the current directory's entries.
  u32 first = 1, end = count;
  std::string_view rest = path;
  while (!rest.empty()) {
    const std::size_t slash = rest.find('/');
    const std::string_view part = rest.substr(0, slash);
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    bool found = false;
    for (u32 i = first; i < end;) {
      const bool dir = table[i * 12] == 1;
      const u32 next = dir ? std::min(be32(i * 12 + 8), end) : i + 1;
      if (name(i) == part) {
        if (dir == rest.empty()) return false;  // a directory where a file was asked for, or the reverse
        if (!dir) {
          *offset = be32(i * 12 + 4);
          *size = be32(i * 12 + 8);
          return u64{*offset} + *size <= size_;
        }
        first = i + 1, end = next, found = true;
        break;
      }
      i = next > i ? next : i + 1;
    }
    if (!found) return false;
  }
  return false;
}

bool Image::read_file(const std::string& path, std::vector<u8>* out) const {
  u64 offset;
  u32 size;
  if (!find_file(path, &offset, &size)) return false;
  out->resize(size);
  return read(offset, out->data(), size);
}

std::string dol_sha1(const Image& img) {
  std::vector<u8> dol;
  return img.read_dol(&dol) ? to_hex(sha1(dol.data(), dol.size())) : std::string();
}

}  // namespace disc
