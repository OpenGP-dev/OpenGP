// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera/jpeg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace camera {

namespace {

// Natural (row-major) position of each coefficient in zigzag order.
constexpr std::array<u8, 64> kZigzag = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The example quantization tables of ITU T.81 annex K (natural order).
constexpr std::array<u8, 64> kLumaQuant = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,
    69, 56, 14, 17, 22,  29,  51,  87,  80, 62, 18, 22, 37,  56,  68,  109, 103, 77, 24, 35, 55, 64,
    81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<u8, 64> kChromaQuant = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// The standard Huffman tables of annex K.3: code counts by length (1-16), then
// the symbols.
struct HuffmanSpec {
  std::array<u8, 16> counts;
  std::vector<u8> symbols;
};

const HuffmanSpec kDcLuma = {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kDcChroma = {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}};
const HuffmanSpec kAcLuma = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D},
    {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
     0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
     0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37,
     0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
     0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
     0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
     0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
     0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
     0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};
const HuffmanSpec kAcChroma = {
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
    {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
     0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
     0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36,
     0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
     0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
     0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
     0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
     0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
     0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA}};

// Code and length of each symbol (canonical Huffman codes).
struct HuffmanCodes {
  std::array<u16, 256> code{};
  std::array<u8, 256> length{};
  explicit HuffmanCodes(const HuffmanSpec& spec) {
    u16 c = 0;
    std::size_t k = 0;
    for (int len = 1; len <= 16; ++len, c <<= 1) {
      for (int i = 0; i < spec.counts[len - 1]; ++i, ++c, ++k) {
        code[spec.symbols[k]] = c;
        length[spec.symbols[k]] = static_cast<u8>(len);
      }
    }
  }
};

class Writer {
 public:
  std::vector<u8> out;

  void byte(u8 b) { out.push_back(b); }
  void word(u16 w) { byte(static_cast<u8>(w >> 8)), byte(static_cast<u8>(w)); }
  void marker(u8 m) { byte(0xFF), byte(m); }

  // Entropy-coded data: most significant bit first, 0xFF followed by 0x00.
  void bits(u32 value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      acc_ = static_cast<u8>((acc_ << 1) | ((value >> i) & 1));
      if (++filled_ == 8) flush_byte();
    }
  }
  void pad() {
    while (filled_) bits(1, 1);  // ones up to the byte's end
  }

 private:
  void flush_byte() {
    byte(acc_);
    if (acc_ == 0xFF) byte(0x00);
    acc_ = 0, filled_ = 0;
  }
  u8 acc_ = 0;
  int filled_ = 0;
};

std::array<u8, 64> scaled_quant(const std::array<u8, 64>& base, int quality) {
  quality = std::clamp(quality, 1, 100);
  const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
  std::array<u8, 64> q;
  for (int i = 0; i < 64; ++i) q[i] = static_cast<u8>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
  return q;
}

void write_quant(Writer& w, u8 id, const std::array<u8, 64>& q) {
  w.marker(0xDB);
  w.word(2 + 1 + 64);
  w.byte(id);
  for (int k = 0; k < 64; ++k) w.byte(q[kZigzag[k]]);
}

void write_huffman(Writer& w, u8 class_id, const HuffmanSpec& spec) {
  w.marker(0xC4);
  w.word(static_cast<u16>(2 + 1 + 16 + spec.symbols.size()));
  w.byte(class_id);
  for (u8 c : spec.counts) w.byte(c);
  for (u8 s : spec.symbols) w.byte(s);
}

// C(u)/2 * cos((2x+1) u pi / 16): the forward DCT is sum f(x,y) t[x][u] t[y][v].
const std::array<std::array<float, 8>, 8>& dct_table() {
  static const auto table = [] {
    std::array<std::array<float, 8>, 8> t{};
    const double pi = 3.14159265358979323846;
    for (int x = 0; x < 8; ++x)
      for (int u = 0; u < 8; ++u)
        t[x][u] = static_cast<float>((u == 0 ? std::sqrt(0.5) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * pi / 16));
    return t;
  }();
  return table;
}

// Bits needed for |v|, and the bits that encode v in that many.
int category(int v) {
  int a = std::abs(v), n = 0;
  while (a) ++n, a >>= 1;
  return n;
}
u32 value_bits(int v, int n) { return static_cast<u32>(v < 0 ? v + (1 << n) - 1 : v); }

class BlockCoder {
 public:
  BlockCoder(Writer& w, const HuffmanCodes& dc, const HuffmanCodes& ac, const std::array<u8, 64>& quant)
      : w_(w), dc_(dc), ac_(ac), quant_(quant) {}

  // `block` holds the samples minus 128, row by row.
  void encode(const std::array<float, 64>& block) {
    const auto& t = dct_table();
    std::array<float, 64> rows{};  // rows[y][u]
    for (int y = 0; y < 8; ++y)
      for (int u = 0; u < 8; ++u) {
        float s = 0;
        for (int x = 0; x < 8; ++x) s += block[y * 8 + x] * t[x][u];
        rows[y * 8 + u] = s;
      }
    std::array<int, 64> q{};  // natural order
    for (int v = 0; v < 8; ++v)
      for (int u = 0; u < 8; ++u) {
        float s = 0;
        for (int y = 0; y < 8; ++y) s += rows[y * 8 + u] * t[y][v];
        q[v * 8 + u] = static_cast<int>(std::lround(s / quant_[v * 8 + u]));
      }

    const int diff = q[0] - previous_dc_;
    previous_dc_ = q[0];
    int n = category(diff);
    symbol(dc_, static_cast<u8>(n));
    w_.bits(value_bits(diff, n), n);

    int run = 0;
    for (int k = 1; k < 64; ++k) {
      const int v = q[kZigzag[k]];
      if (v == 0) {
        ++run;
        continue;
      }
      for (; run > 15; run -= 16) symbol(ac_, 0xF0);  // sixteen zeros
      n = category(v);
      symbol(ac_, static_cast<u8>((run << 4) | n));
      w_.bits(value_bits(v, n), n);
      run = 0;
    }
    if (run) symbol(ac_, 0x00);  // end of block
  }

 private:
  void symbol(const HuffmanCodes& h, u8 s) { w_.bits(h.code[s], h.length[s]); }

  Writer& w_;
  const HuffmanCodes& dc_;
  const HuffmanCodes& ac_;
  const std::array<u8, 64>& quant_;
  int previous_dc_ = 0;
};

}  // namespace

std::vector<u8> encode_jpeg(const u8* rgb, int width, int height, int quality) {
  const auto luma_q = scaled_quant(kLumaQuant, quality);
  const auto chroma_q = scaled_quant(kChromaQuant, quality);
  static const HuffmanCodes dc_luma(kDcLuma), ac_luma(kAcLuma), dc_chroma(kDcChroma), ac_chroma(kAcChroma);

  Writer w;
  w.out.reserve(static_cast<std::size_t>(width) * height / 2);
  w.marker(0xD8);  // SOI
  w.marker(0xE0);  // APP0: JFIF 1.01, no density, no thumbnail
  w.word(16);
  for (u8 b : {'J', 'F', 'I', 'F', '\0'}) w.byte(b);
  for (u8 b : {1, 1, 0, 0, 1, 0, 1, 0, 0}) w.byte(b);
  write_quant(w, 0, luma_q);
  write_quant(w, 1, chroma_q);
  w.marker(0xC0);  // SOF0: 8 bits, Y at 2x2 with table 0, Cb and Cr at 1x1 with table 1
  w.word(17);
  w.byte(8);
  w.word(static_cast<u16>(height));
  w.word(static_cast<u16>(width));
  w.byte(3);
  for (u8 b : {1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1}) w.byte(b);
  write_huffman(w, 0x00, kDcLuma);
  write_huffman(w, 0x10, kAcLuma);
  write_huffman(w, 0x01, kDcChroma);
  write_huffman(w, 0x11, kAcChroma);
  w.marker(0xDA);  // SOS: all three components
  w.word(12);
  w.byte(3);
  for (u8 b : {1, 0x00, 2, 0x11, 3, 0x11}) w.byte(b);
  for (u8 b : {0, 63, 0}) w.byte(b);

  BlockCoder y_coder(w, dc_luma, ac_luma, luma_q);
  BlockCoder cb_coder(w, dc_chroma, ac_chroma, chroma_q);
  BlockCoder cr_coder(w, dc_chroma, ac_chroma, chroma_q);
  // The picture as YCbCr (JFIF), edges repeated to whole 16x16 MCUs.
  auto sample = [&](int x, int y, float* ycc) {
    const u8* p = rgb + (static_cast<std::size_t>(std::min(y, height - 1)) * width + std::min(x, width - 1)) * 3;
    const float r = p[0], g = p[1], b = p[2];
    ycc[0] = 0.299f * r + 0.587f * g + 0.114f * b;
    ycc[1] = -0.168736f * r - 0.331264f * g + 0.5f * b + 128;
    ycc[2] = 0.5f * r - 0.418688f * g - 0.081312f * b + 128;
  };
  std::array<float, 16 * 16 * 3> mcu;
  std::array<float, 64> block;
  for (int my = 0; my < height; my += 16) {
    for (int mx = 0; mx < width; mx += 16) {
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) sample(mx + x, my + y, &mcu[(y * 16 + x) * 3]);
      for (int by = 0; by < 16; by += 8)
        for (int bx = 0; bx < 16; bx += 8) {
          for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) block[y * 8 + x] = mcu[((by + y) * 16 + bx + x) * 3] - 128;
          y_coder.encode(block);
        }
      for (int c = 1; c <= 2; ++c) {
        for (int y = 0; y < 8; ++y)
          for (int x = 0; x < 8; ++x) {
            const int i = (y * 2 * 16 + x * 2) * 3 + c;
            block[y * 8 + x] = (mcu[i] + mcu[i + 3] + mcu[i + 16 * 3] + mcu[i + 17 * 3]) / 4 - 128;
          }
        (c == 1 ? cb_coder : cr_coder).encode(block);
      }
    }
  }
  w.pad();
  w.marker(0xD9);  // EOI
  return std::move(w.out);
}

}  // namespace camera
