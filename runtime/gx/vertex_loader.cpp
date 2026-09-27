// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/vertex_loader.h"

#include <bit>
#include <cstring>

namespace gx {

namespace {

enum : u32 { NOT_PRESENT = 0, DIRECT = 1, INDEX8 = 2, INDEX16 = 3 };

// Array numbers (CP_ARRAY_BASE + n).
enum : u32 { ARRAY_POSITION = 0, ARRAY_NORMAL = 1, ARRAY_COLOR0 = 2, ARRAY_TEX0 = 4 };

constexpr u32 kComponentSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};  // u8, s8, u16, s16, f32 (5-7 act as f32)
constexpr u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};      // 565, 888, 888x, 4444, 6666, 8888

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }

float component(const u8* p, u32 format, float scale) {
  switch (format) {
    case 0: return p[0] * scale;
    case 1: return static_cast<s8>(p[0]) * scale;
    case 2: return be16(p) * scale;
    case 3: return static_cast<s16>(be16(p)) * scale;
    default: return std::bit_cast<float>(be32(p));
  }
}

u32 index_size(u32 type) { return type == INDEX8 ? 1 : type == INDEX16 ? 2 : 0; }

u8 expand4(u32 v) { return static_cast<u8>(v * 17); }
u8 expand5(u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); }
u8 expand6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }

std::array<u8, 4> read_color(const u8* p, u32 format) {
  switch (format) {
    case 0: {  // RGB565
      const u32 v = be16(p);
      return {expand5(v >> 11), expand6((v >> 5) & 0x3F), expand5(v & 0x1F), 0xFF};
    }
    case 1:  // RGB888
    case 2:  // RGB888x
      return {p[0], p[1], p[2], 0xFF};
    case 3:  // RGBA4444
      return {expand4(p[0] >> 4), expand4(p[0] & 0xF), expand4(p[1] >> 4), expand4(p[1] & 0xF)};
    case 4: {  // RGBA6666
      const u32 v = (u32{p[0]} << 16) | (u32{p[1]} << 8) | p[2];
      return {expand6(v >> 18), expand6((v >> 12) & 0x3F), expand6((v >> 6) & 0x3F), expand6(v & 0x3F)};
    }
    default:  // RGBA8888
      return {p[0], p[1], p[2], p[3]};
  }
}

}  // namespace

void VertexLoader::setup(const u32* cp, u8 vat) {
  const u32 lo = cp[0x50], hi = cp[0x60];
  const u32 a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];

  matrix_flags_ = lo & 0x1FF;
  size_ = static_cast<u32>(std::popcount(matrix_flags_));

  auto frac_scale = [](u32 frac) { return 1.0f / static_cast<float>(1u << frac); };

  pos_ = {bits(lo, 9, 2), bits(a, 1, 3), bits(a, 0, 1) ? 3u : 2u, frac_scale(bits(a, 4, 5))};

  normal_ = {bits(lo, 11, 2), bits(a, 10, 3), bits(a, 9, 1) ? 9u : 3u, 1.0f};
  switch (normal_.format) {  // normals have a fixed point position per format
    case 0: normal_.scale = 1.0f / 128; break;
    case 1: normal_.scale = 1.0f / 64; break;
    case 2: normal_.scale = 1.0f / 32768; break;
    case 3: normal_.scale = 1.0f / 16384; break;
    default: break;
  }
  normal_index3_ = normal_.count == 9 && bits(a, 31, 1);

  for (u32 i = 0; i < 2; ++i) color_[i] = {bits(lo, 13 + 2 * i, 2), bits(a, 14 + 4 * i, 3), 1, 1.0f};

  // Texture coordinate fields: count bit, then 3 format bits, then 5 fraction bits.
  struct Field {
    u32 word, shift, frac_word, frac_shift;
  };
  const Field fields[8] = {{a, 21, a, 25}, {b, 0, b, 4},   {b, 9, b, 13},  {b, 18, b, 22},
                           {b, 27, c, 0},  {c, 5, c, 9},   {c, 14, c, 18}, {c, 23, c, 27}};
  for (u32 i = 0; i < 8; ++i) {
    const Field& f = fields[i];
    tex_[i] = {bits(hi, 2 * i, 2), bits(f.word, f.shift + 1, 3), bits(f.word, f.shift, 1) ? 2u : 1u,
               frac_scale(bits(f.frac_word, f.frac_shift, 5))};
  }

  auto attr_size = [](const Attr& at, u32 direct_size, u32 indices) {
    if (at.type == DIRECT) return direct_size;
    return index_size(at.type) * indices;
  };
  size_ += attr_size(pos_, pos_.count * kComponentSize[pos_.format], 1);
  size_ += attr_size(normal_, normal_.count * kComponentSize[normal_.format], normal_index3_ ? 3 : 1);
  for (const Attr& col : color_) size_ += attr_size(col, kColorSize[col.format], 1);
  for (const Attr& t : tex_) size_ += attr_size(t, t.count * kComponentSize[t.format], 1);

  for (u32 i = 0; i < 16; ++i) {
    array_base_[i] = cp[0xA0 + i] & 0x03FFFFFF;
    array_stride_[i] = cp[0xB0 + i] & 0xFF;
  }
}

// Returns the attribute's data: in the stream (direct) or in the array
// (indexed). Advances `src` past what the stream holds.
const u8* VertexLoader::fetch(const Attr& a, u32 array, const u8*& src, u32 bytes, MemoryReader read,
                              void* context) const {
  if (a.type == DIRECT) {
    const u8* p = src;
    src += bytes;
    return p;
  }
  const u32 index = a.type == INDEX8 ? *src : be16(src);
  src += index_size(a.type);
  return read(context, array_base_[array] + index * array_stride_[array], bytes);
}

void VertexLoader::load(const u8* src, InputVertex& v, MemoryReader read, void* context, u32 matrix_index_a,
                        u32 matrix_index_b) const {
  // Matrix indices: from the vertex if present, else the XF registers.
  const u32 default_tex[8] = {bits(matrix_index_a, 6, 6),  bits(matrix_index_a, 12, 6), bits(matrix_index_a, 18, 6),
                              bits(matrix_index_a, 24, 6), bits(matrix_index_b, 0, 6),  bits(matrix_index_b, 6, 6),
                              bits(matrix_index_b, 12, 6), bits(matrix_index_b, 18, 6)};
  v.posmtx = static_cast<u8>((matrix_flags_ & 1) ? (*src++ & 0x3F) : bits(matrix_index_a, 0, 6));
  for (u32 i = 0; i < 8; ++i)
    v.texmtx[i] = static_cast<u8>((matrix_flags_ & (2u << i)) ? (*src++ & 0x3F) : default_tex[i]);

  // Position. An index of all ones means "skip this vertex".
  v.skip = false;
  const u32 pos_bytes = pos_.count * kComponentSize[pos_.format];
  if (pos_.type == INDEX8 && *src == 0xFF) v.skip = true;
  if (pos_.type == INDEX16 && be16(src) == 0xFFFF) v.skip = true;
  if (const u8* p = fetch(pos_, ARRAY_POSITION, src, pos_bytes, read, context)) {
    const u32 step = kComponentSize[pos_.format];
    v.pos[0] = component(p, pos_.format, pos_.scale);
    v.pos[1] = component(p + step, pos_.format, pos_.scale);
    v.pos[2] = pos_.count == 3 ? component(p + 2 * step, pos_.format, pos_.scale) : 0.0f;
  } else {
    v.skip = true;
  }

  // Normal (and tangent/binormal). Absent normals keep the last values loaded.
  if (normal_.type != NOT_PRESENT) {
    const u32 step = kComponentSize[normal_.format];
    auto read_vec = [&](const u8* p, float* out) {
      for (u32 k = 0; k < 3; ++k) out[k] = component(p + k * step, normal_.format, normal_.scale);
    };
    if (normal_index3_ && normal_.type != DIRECT) {
      for (u32 k = 0; k < 3; ++k) {
        // Each index selects a whole NBT entry; take its k-th vector.
        if (const u8* p = fetch(normal_, ARRAY_NORMAL, src, 9 * step, read, context)) read_vec(p + 3 * k * step, v.normal[k]);
      }
    } else if (const u8* p = fetch(normal_, ARRAY_NORMAL, src, normal_.count * step, read, context)) {
      read_vec(p, v.normal[0]);
      if (normal_.count == 9) {
        read_vec(p + 3 * step, v.normal[1]);
        read_vec(p + 6 * step, v.normal[2]);
      }
    }
  }

  // Colours: colour 0 feeds channel 0 and colour 1 channel 1 when both are
  // present; a lone colour 1 feeds channel 0. Missing colours are white.
  std::array<u8, 4> colors[2] = {{0xFF, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF}};
  bool present[2] = {false, false};
  for (u32 i = 0; i < 2; ++i) {
    if (color_[i].type == NOT_PRESENT) continue;
    if (const u8* p = fetch(color_[i], ARRAY_COLOR0 + i, src, kColorSize[color_[i].format], read, context)) {
      colors[i] = read_color(p, color_[i].format);
      present[i] = true;
    }
  }
  if (present[0]) {
    v.color[0] = colors[0];
    v.color[1] = present[1] ? colors[1] : std::array<u8, 4>{0xFF, 0xFF, 0xFF, 0xFF};
  } else {
    v.color[0] = present[1] ? colors[1] : std::array<u8, 4>{0xFF, 0xFF, 0xFF, 0xFF};
    v.color[1] = {0xFF, 0xFF, 0xFF, 0xFF};
  }

  // Texture coordinates.
  for (u32 i = 0; i < 8; ++i) {
    const Attr& t = tex_[i];
    v.tex[i][0] = v.tex[i][1] = 0.0f;
    if (t.type == NOT_PRESENT) continue;
    const u32 step = kComponentSize[t.format];
    if (const u8* p = fetch(t, ARRAY_TEX0 + i, src, t.count * step, read, context)) {
      v.tex[i][0] = component(p, t.format, t.scale);
      if (t.count == 2) v.tex[i][1] = component(p + step, t.format, t.scale);
    }
  }
}

}  // namespace gx
