// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/vertex_loader.h"

#include <cstring>

namespace gx {

namespace {

constexpr std::array<u8, 4> kWhite{0xFF, 0xFF, 0xFF, 0xFF};

}  // namespace

void VertexLoader::setup(const u32* cp, u8 vat, const u8* ram, u32 ram_size) {
  // The words the setup depends on: the vertex descriptor, this attribute
  // table and the arrays.
  u32 key[5 + 32];
  key[0] = cp[0x50], key[1] = cp[0x60];
  key[2] = cp[0x70 + vat], key[3] = cp[0x80 + vat], key[4] = cp[0x90 + vat];
  std::memcpy(key + 5, cp + 0xA0, 32 * sizeof(u32));
  if (keyed_ && ram == key_ram_ && ram_size == key_ram_size_ && std::memcmp(key, key_, sizeof(key)) == 0) return;
  std::memcpy(key_, key, sizeof(key));
  key_ram_ = ram, key_ram_size_ = ram_size, keyed_ = true;

  ram_ = ram;
  ram_size_ = ram_size;
  const u32 lo = cp[0x50], hi = cp[0x60];
  const u32 a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];

  matrix_flags_ = lo & 0x1FF;
  size_ = static_cast<u32>(std::popcount(matrix_flags_));

  auto frac_scale = [](u32 frac) { return 1.0f / static_cast<float>(1u << frac); };

  pos_ = {bits(lo, 9, 2), bits(a, 1, 3), bits(a, 0, 1) ? 3u : 2u, frac_scale(bits(a, 4, 5))};
  pos_bytes_ = pos_.count * kComponentSize[pos_.format];

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
  colors_present_ = color_[0].type != NOT_PRESENT || color_[1].type != NOT_PRESENT;

  // Texture coordinate fields: count bit, then 3 format bits, then 5 fraction bits.
  struct Field {
    u32 word, shift, frac_word, frac_shift;
  };
  const Field fields[8] = {{a, 21, a, 25}, {b, 0, b, 4},   {b, 9, b, 13},  {b, 18, b, 22},
                           {b, 27, c, 0},  {c, 5, c, 9},   {c, 14, c, 18}, {c, 23, c, 27}};
  num_tex_slots_ = 0;
  for (u32 i = 0; i < 8; ++i) {
    const Field& f = fields[i];
    tex_[i] = {bits(hi, 2 * i, 2), bits(f.word, f.shift + 1, 3), bits(f.word, f.shift, 1) ? 2u : 1u,
               frac_scale(bits(f.frac_word, f.frac_shift, 5))};
    if (tex_[i].type != NOT_PRESENT) tex_slots_[num_tex_slots_++] = i;
  }

  auto attr_size = [](const Attr& at, u32 direct_size, u32 indices) {
    if (at.type == DIRECT) return direct_size;
    return index_size(at.type) * indices;
  };
  size_ += attr_size(pos_, pos_bytes_, 1);
  size_ += attr_size(normal_, normal_.count * kComponentSize[normal_.format], normal_index3_ ? 3 : 1);
  for (const Attr& col : color_) size_ += attr_size(col, kColorSize[col.format], 1);
  for (const Attr& t : tex_) size_ += attr_size(t, t.count * kComponentSize[t.format], 1);

  for (u32 i = 0; i < 16; ++i) {
    array_base_[i] = cp[0xA0 + i] & 0x03FFFFFF;
    array_stride_[i] = cp[0xB0 + i] & 0xFF;
  }
}

void VertexLoader::begin(u32 matrix_index_a, u32 matrix_index_b, InputVertex& v) const {
  default_posmtx_ = static_cast<u8>(bits(matrix_index_a, 0, 6));
  const u32 tex[8] = {bits(matrix_index_a, 6, 6),  bits(matrix_index_a, 12, 6), bits(matrix_index_a, 18, 6),
                      bits(matrix_index_a, 24, 6), bits(matrix_index_b, 0, 6),  bits(matrix_index_b, 6, 6),
                      bits(matrix_index_b, 12, 6), bits(matrix_index_b, 18, 6)};
  for (u32 i = 0; i < 8; ++i) default_texmtx_[i] = static_cast<u8>(tex[i]);
  if (!matrix_flags_) {  // the same for every vertex: load() leaves them alone
    v.posmtx = default_posmtx_;
    for (u32 i = 0; i < 8; ++i) v.texmtx[i] = default_texmtx_[i];
  }
  // Attributes the vertices lack, which load() doesn't touch.
  for (u32 i = 0; i < 8; ++i)
    if (tex_[i].type == NOT_PRESENT) v.tex[i][0] = v.tex[i][1] = 0.0f;
  if (!colors_present_) v.color[0] = v.color[1] = kWhite;
}

void VertexLoader::load_normals(const u8*& src, InputVertex& v) const {
  // Normal (and tangent/binormal). Absent normals keep the last values loaded.
  const u32 step = kComponentSize[normal_.format];
  auto read_vec = [&](const u8* p, float* out) {
    for (u32 k = 0; k < 3; ++k) out[k] = component(p + k * step, normal_.format, normal_.scale);
  };
  if (normal_index3_ && normal_.type != DIRECT) {
    for (u32 k = 0; k < 3; ++k) {
      // Each index selects a whole NBT entry; take its k-th vector.
      if (const u8* p = fetch(normal_, ARRAY_NORMAL, src, 9 * step)) read_vec(p + 3 * k * step, v.normal[k]);
    }
  } else if (const u8* p = fetch(normal_, ARRAY_NORMAL, src, normal_.count * step)) {
    read_vec(p, v.normal[0]);
    if (normal_.count == 9) {
      read_vec(p + 3 * step, v.normal[1]);
      read_vec(p + 6 * step, v.normal[2]);
    }
  }
}

}  // namespace gx
