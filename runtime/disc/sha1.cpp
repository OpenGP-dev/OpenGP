// SPDX-License-Identifier: GPL-3.0-or-later

#include "disc/sha1.h"

#include <cstring>

namespace disc {

namespace {

using gekko::u32;
using gekko::u64;
using gekko::u8;

u32 rotl(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

void process_block(u32 h[5], const u8* block) {
  u32 w[80];
  for (int i = 0; i < 16; ++i)
    w[i] = (u32{block[4 * i]} << 24) | (u32{block[4 * i + 1]} << 16) | (u32{block[4 * i + 2]} << 8) | block[4 * i + 3];
  for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    u32 f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    const u32 t = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

Sha1Digest sha1(const void* data, std::size_t size) {
  u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  const u8* p = static_cast<const u8*>(data);
  std::size_t full = size / 64 * 64;
  for (std::size_t i = 0; i < full; i += 64) process_block(h, p + i);

  // The tail: the remaining bytes, 0x80, zeros, and the length in bits.
  u8 tail[128] = {};
  const std::size_t rest = size - full;
  if (rest) std::memcpy(tail, p + full, rest);
  tail[rest] = 0x80;
  const std::size_t tail_size = rest < 56 ? 64 : 128;
  const u64 bits = static_cast<u64>(size) * 8;
  for (int i = 0; i < 8; ++i) tail[tail_size - 1 - i] = static_cast<u8>(bits >> (8 * i));
  for (std::size_t i = 0; i < tail_size; i += 64) process_block(h, tail + i);

  Sha1Digest out;
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<u8>(h[i] >> (24 - 8 * j));
  return out;
}

std::string to_hex(const Sha1Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (u8 b : digest) {
    s += kHex[b >> 4];
    s += kHex[b & 15];
  }
  return s;
}

}  // namespace disc
