// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal PNG writer for screenshots: RGBA8, zlib "stored" (uncompressed)
// blocks, so it needs no compression library.

#include <algorithm>
#include <array>
#include <cstdio>

#include "video/renderer.h"
#include "os/utf8.h"

namespace video {

namespace {

u32 crc32(const u8* p, std::size_t n, u32 crc = 0) {
  static const std::array<u32, 256> table = [] {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<u8>& v, u32 x) {
  v.push_back(static_cast<u8>(x >> 24));
  v.push_back(static_cast<u8>(x >> 16));
  v.push_back(static_cast<u8>(x >> 8));
  v.push_back(static_cast<u8>(x));
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
  put32(out, static_cast<u32>(data.size()));
  std::vector<u8> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put32(out, crc32(body.data(), body.size()));
}

}  // namespace

bool write_png(const std::string& path, const Image& image) {
  if (image.width == 0 || image.height == 0) return false;
  // Raw scanlines, each with filter type 0.
  std::vector<u8> raw;
  const std::size_t stride = std::size_t{image.width} * 4;
  raw.reserve((stride + 1) * image.height);
  for (u32 y = 0; y < image.height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(y * stride),
               image.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride));
  }
  // zlib stream of stored blocks.
  std::vector<u8> z = {0x78, 0x01};
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
    z.push_back(pos + n == raw.size() ? 1 : 0);
    z.push_back(static_cast<u8>(n));
    z.push_back(static_cast<u8>(n >> 8));
    z.push_back(static_cast<u8>(~n));
    z.push_back(static_cast<u8>(~n >> 8));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
  }
  u32 a = 1, b = 0;
  for (u8 c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);

  std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<u8> ihdr;
  put32(ihdr, image.width);
  put32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});

  std::FILE* f = os::fopen_utf8(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

}  // namespace video
