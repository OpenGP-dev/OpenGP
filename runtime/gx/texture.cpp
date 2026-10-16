// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/texture.h"

#include <algorithm>
#include <cstring>

namespace gx {

namespace {

u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }
u32 c3(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }
u32 c4(u32 v) { return (v << 4) | v; }
u32 c5(u32 v) { return (v << 3) | (v >> 2); }
u32 c6(u32 v) { return (v << 2) | (v >> 4); }
u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }

// Two bytes, alpha first.
u32 decode_ia8(const u8* p) { return rgba(p[1], p[1], p[1], p[0]); }
u32 decode_rgb565(u32 v) { return rgba(c5(v >> 11), c6((v >> 5) & 0x3F), c5(v & 0x1F), 0xFF); }
u32 decode_rgb5a3(u32 v) {
  if (v & 0x8000) return rgba(c5((v >> 10) & 0x1F), c5((v >> 5) & 0x1F), c5(v & 0x1F), 0xFF);
  return rgba(c4((v >> 8) & 0xF), c4((v >> 4) & 0xF), c4(v & 0xF), c3((v >> 12) & 0x7));
}

u32 decode_palette(const u8* tlut, u32 index, TlutFormat format) {
  const u8* entry = tlut + 2 * index;
  switch (format) {
    case TlutFormat::IA8: return decode_ia8(entry);
    case TlutFormat::RGB565: return decode_rgb565(be16(entry));
    case TlutFormat::RGB5A3: return decode_rgb5a3(be16(entry));
    default: return 0;
  }
}

// One 4x4 CMPR (S3TC-like) sub-block. The GameCube decodes it differently
// from PC DXT1: colour 3 in the 3-colour mode is the average, transparent.
void decode_dxt_block(u32* dst, const u8* src, u32 pitch) {
  const u32 c1 = be16(src), c2 = be16(src + 2);
  const u32 b1 = c5(c1 & 0x1F), b2 = c5(c2 & 0x1F);
  const u32 g1 = c6((c1 >> 5) & 0x3F), g2 = c6((c2 >> 5) & 0x3F);
  const u32 r1 = c5((c1 >> 11) & 0x1F), r2 = c5((c2 >> 11) & 0x1F);
  auto blend = [](u32 a, u32 b) { return (a * 3 + b * 5) >> 3; };  // 3/8 blend, close to 1/3
  u32 colors[4];
  colors[0] = rgba(r1, g1, b1, 255);
  colors[1] = rgba(r2, g2, b2, 255);
  if (c1 > c2) {
    colors[2] = rgba(blend(r2, r1), blend(g2, g1), blend(b2, b1), 255);
    colors[3] = rgba(blend(r1, r2), blend(g1, g2), blend(b1, b2), 255);
  } else {
    colors[2] = rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 255);
    colors[3] = rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 0);
  }
  for (u32 y = 0; y < 4; ++y) {
    u32 val = src[4 + y];
    for (u32 x = 0; x < 4; ++x) {
      dst[x] = colors[(val >> 6) & 3];
      val <<= 2;
    }
    dst += pitch;
  }
}

// Decodes a whole block-padded level of `width` x `height` (multiples of
// the block size) into `dst` with a pitch of `width`.
void decode_padded(u32* dst, const u8* src, TexFormat format, u32 width, u32 height, const u8* tlut, TlutFormat tf) {
  const u32 w4 = (width + 3) / 4, w8 = (width + 7) / 8;
  switch (format) {
    case TexFormat::I4:
      for (u32 y = 0; y < height; y += 8)
        for (u32 x = 0; x < width; x += 8)
          for (u32 iy = 0; iy < 8; ++iy, src += 4)
            for (u32 ix = 0; ix < 4; ++ix) {
              const u32 i1 = c4(src[ix] >> 4), i2 = c4(src[ix] & 0xF);
              dst[(y + iy) * width + x + ix * 2] = rgba(i1, i1, i1, i1);
              dst[(y + iy) * width + x + ix * 2 + 1] = rgba(i2, i2, i2, i2);
            }
      break;
    case TexFormat::I8:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 8)
          for (u32 iy = 0; iy < 4; ++iy, src += 8)
            for (u32 ix = 0; ix < 8; ++ix) dst[(y + iy) * width + x + ix] = rgba(src[ix], src[ix], src[ix], src[ix]);
      break;
    case TexFormat::IA4:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 8)
          for (u32 iy = 0; iy < 4; ++iy, src += 8)
            for (u32 ix = 0; ix < 8; ++ix) {
              const u32 a = c4(src[ix] >> 4), l = c4(src[ix] & 0xF);
              dst[(y + iy) * width + x + ix] = rgba(l, l, l, a);
            }
      break;
    case TexFormat::IA8:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 4)
          for (u32 iy = 0; iy < 4; ++iy, src += 8)
            for (u32 ix = 0; ix < 4; ++ix) dst[(y + iy) * width + x + ix] = decode_ia8(src + 2 * ix);
      break;
    case TexFormat::RGB565:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 4)
          for (u32 iy = 0; iy < 4; ++iy, src += 8)
            for (u32 ix = 0; ix < 4; ++ix) dst[(y + iy) * width + x + ix] = decode_rgb565(be16(src + 2 * ix));
      break;
    case TexFormat::RGB5A3:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 4)
          for (u32 iy = 0; iy < 4; ++iy, src += 8)
            for (u32 ix = 0; ix < 4; ++ix) dst[(y + iy) * width + x + ix] = decode_rgb5a3(be16(src + 2 * ix));
      break;
    case TexFormat::RGBA8:
      // 64-byte blocks: 16 alpha/red pairs, then 16 green/blue pairs.
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0; x < width; x += 4, src += 64)
          for (u32 iy = 0; iy < 4; ++iy)
            for (u32 ix = 0; ix < 4; ++ix) {
              const u8* ar = src + 2 * (4 * iy + ix);
              const u8* gb = ar + 32;
              dst[(y + iy) * width + x + ix] = rgba(ar[1], gb[0], gb[1], ar[0]);
            }
      break;
    case TexFormat::C4:
      for (u32 y = 0; y < height; y += 8)
        for (u32 x = 0, ystep = (y / 8) * w8; x < width; x += 8, ++ystep)
          for (u32 iy = 0, xstep = 8 * ystep; iy < 8; ++iy, ++xstep)
            for (u32 ix = 0; ix < 4; ++ix) {
              const u8 v = src[4 * xstep + ix];
              dst[(y + iy) * width + x + 2 * ix] = decode_palette(tlut, v >> 4, tf);
              dst[(y + iy) * width + x + 2 * ix + 1] = decode_palette(tlut, v & 0xF, tf);
            }
      break;
    case TexFormat::C8:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0, ystep = (y / 4) * w8; x < width; x += 8, ++ystep)
          for (u32 iy = 0, xstep = 4 * ystep; iy < 4; ++iy, ++xstep)
            for (u32 ix = 0; ix < 8; ++ix) dst[(y + iy) * width + x + ix] = decode_palette(tlut, src[8 * xstep + ix], tf);
      break;
    case TexFormat::C14X2:
      for (u32 y = 0; y < height; y += 4)
        for (u32 x = 0, ystep = (y / 4) * w4; x < width; x += 4, ++ystep)
          for (u32 iy = 0, xstep = 4 * ystep; iy < 4; ++iy, ++xstep)
            for (u32 ix = 0; ix < 4; ++ix)
              dst[(y + iy) * width + x + ix] = decode_palette(tlut, be16(src + 8 * xstep + 2 * ix) & 0x3FFF, tf);
      break;
    case TexFormat::CMPR:
      for (u32 y = 0; y < height; y += 8)
        for (u32 x = 0; x < width; x += 8) {
          decode_dxt_block(dst + y * width + x, src, width);
          decode_dxt_block(dst + y * width + x + 4, src + 8, width);
          decode_dxt_block(dst + (y + 4) * width + x, src + 16, width);
          decode_dxt_block(dst + (y + 4) * width + x + 4, src + 24, width);
          src += 32;
        }
      break;
  }
}

}  // namespace

u32 block_width(TexFormat format) {
  switch (format) {
    case TexFormat::I4:
    case TexFormat::I8:
    case TexFormat::IA4:
    case TexFormat::C4:
    case TexFormat::C8:
    case TexFormat::CMPR: return 8;
    default: return 4;
  }
}

u32 block_height(TexFormat format) {
  switch (format) {
    case TexFormat::I4:
    case TexFormat::C4:
    case TexFormat::CMPR: return 8;
    default: return 4;
  }
}

u32 texture_level_size(TexFormat format, u32 width, u32 height) {
  const u32 bw = block_width(format), bh = block_height(format);
  const u32 blocks = ((width + bw - 1) / bw) * ((height + bh - 1) / bh);
  return blocks * (format == TexFormat::RGBA8 ? 64 : 32);
}

u32 texture_size(TexFormat format, u32 width, u32 height, u32 levels) {
  u32 size = 0;
  for (u32 l = 0; l < levels; ++l) {
    size += texture_level_size(format, width, height);
    width = width > 1 ? width / 2 : 1;
    height = height > 1 ? height / 2 : 1;
  }
  return size;
}

bool is_paletted(TexFormat format) {
  return format == TexFormat::C4 || format == TexFormat::C8 || format == TexFormat::C14X2;
}

u32 palette_entries(TexFormat format) {
  switch (format) {
    case TexFormat::C4: return 16;
    case TexFormat::C8: return 256;
    case TexFormat::C14X2: return 16384;
    default: return 0;
  }
}

bool is_valid_format(u32 format) { return format <= 0x6 || format == 0x8 || format == 0x9 || format == 0xA || format == 0xE; }

void decode_texture(u32* dst, const u8* src, TexFormat format, u32 width, u32 height, const u8* tlut,
                    TlutFormat tlut_format, std::span<u32> scratch) {
  const u32 bw = block_width(format), bh = block_height(format);
  const u32 pw = (width + bw - 1) / bw * bw, ph = (height + bh - 1) / bh * bh;
  if (pw == width && ph == height) {
    decode_padded(dst, src, format, pw, ph, tlut, tlut_format);
    return;
  }
  decode_padded(scratch.data(), src, format, pw, ph, tlut, tlut_format);
  for (u32 y = 0; y < height; ++y) std::memcpy(dst + y * width, scratch.data() + y * pw, width * sizeof(u32));
}

TexFormat copy_texture_format(CopyFormat format) {
  switch (format) {
    case CopyFormat::R4: return TexFormat::I4;
    case CopyFormat::RA4: return TexFormat::IA4;
    case CopyFormat::RA8:
    case CopyFormat::RG8:
    case CopyFormat::GB8: return TexFormat::IA8;
    case CopyFormat::RGB565: return TexFormat::RGB565;
    case CopyFormat::RGB5A3: return TexFormat::RGB5A3;
    case CopyFormat::RGBA8:
    case CopyFormat::XFB: return TexFormat::RGBA8;
    default: return TexFormat::I8;  // R8, A8, G8, B8
  }
}

bool fades_with_distance(const u32* texels, u32 width, u32 height, u32 levels) {
  // Level 0 nearly all transparent (99%), a smaller level visible (1%).
  auto transparent = [](const u32* level, std::size_t n) {
    std::size_t clear = 0;
    for (std::size_t i = 0; i < n; ++i) clear += (level[i] >> 24) == 0;
    return clear;
  };
  const std::size_t n0 = std::size_t{width} * height;
  if (transparent(texels, n0) * 100 < n0 * 99) return false;
  const u32* level = texels + n0;
  for (u32 l = 1, w = std::max(1u, width / 2), h = std::max(1u, height / 2); l < levels; ++l) {
    const std::size_t n = std::size_t{w} * h;
    if ((n - transparent(level, n)) * 100 >= n) return true;
    level += n;
    w = std::max(1u, w / 2), h = std::max(1u, h / 2);
  }
  return false;
}

}  // namespace gx
