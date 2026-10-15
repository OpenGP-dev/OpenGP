// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vertex loader: turns the vertices of a GX primitive (in the format the
// CP vertex descriptor and attribute table describe) into InputVertex,
// resolving indexed attributes through the CP arrays in guest memory.
//
// load() runs for every vertex the game draws (over 80,000 a frame on the
// busiest courses), so it is defined here, for the draw loop to inline, and
// everything that is the same for a whole draw is worked out once, by
// setup() and begin().
//
// Based on Dolphin's VertexLoader*.cpp and the software renderer's
// SWVertexLoader.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <array>
#include <bit>

#include "gx/regs.h"

namespace gx {

struct InputVertex {
  u8 posmtx;                  // position/normal matrix row (position matrices are 4 floats per row)
  u8 texmtx[8];               // texture matrix rows
  float pos[3];
  float normal[3][3];         // normal, tangent, binormal
  std::array<u8, 4> color[2];  // lighting channel inputs, RGBA
  float tex[8][2];
  bool skip;                  // indexed position with the "skip" index (all ones)
};

class VertexLoader {
 public:
  // Reads guest memory: returns a pointer to `size` bytes at physical
  // address `addr`, or nullptr.
  using MemoryReader = const u8* (*)(void* context, u32 addr, u32 size);

  // Configures the loader for vertex attribute table `vat` from the CP
  // registers (indexed by CP register address). Indexed attributes are read
  // from `ram` (guest MEM1, `ram_size` bytes). Cheap when nothing changed.
  void setup(const u32* cp, u8 vat, const u8* ram, u32 ram_size);

  // Bytes per vertex in the command stream.
  u32 size() const { return size_; }

  // Starts a draw: `matrix_index_a/b` are the XF matrix index registers,
  // used when the vertices have no matrix indices. `v` is the vertex the
  // draw's load() calls fill.
  void begin(u32 matrix_index_a, u32 matrix_index_b, InputVertex& v) const;

  // Decodes one vertex from `src` (after begin()). Attributes the vertex
  // lacks keep their previous values in `v` (normals), or get defaults
  // (colours: white; texture coordinates: 0).
  void load(const u8* src, InputVertex& v) const;

  bool has_normal() const { return normal_.type != 0; }

 private:
  enum : u32 { NOT_PRESENT = 0, DIRECT = 1, INDEX8 = 2, INDEX16 = 3 };
  // Array numbers (CP_ARRAY_BASE + n).
  enum : u32 { ARRAY_POSITION = 0, ARRAY_NORMAL = 1, ARRAY_COLOR0 = 2, ARRAY_TEX0 = 4 };
  static constexpr u32 kComponentSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};  // u8, s8, u16, s16, f32 (5-7 act as f32)
  static constexpr u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};      // 565, 888, 888x, 4444, 6666, 8888

  struct Attr {
    u32 type = 0;     // 0 not present, 1 direct, 2 index8, 3 index16
    u32 format = 0;   // component or colour format
    u32 count = 0;    // components
    float scale = 1;  // dequantisation for integer formats
  };

  static u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
  static u32 be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }
  static float component(const u8* p, u32 format, float scale) {
    switch (format) {
      case 0: return p[0] * scale;
      case 1: return static_cast<s8>(p[0]) * scale;
      case 2: return be16(p) * scale;
      case 3: return static_cast<s16>(be16(p)) * scale;
      default: return std::bit_cast<float>(be32(p));
    }
  }
  static u32 index_size(u32 type) { return type == INDEX8 ? 1 : type == INDEX16 ? 2 : 0; }
  static std::array<u8, 4> read_color(const u8* p, u32 format);

  // Returns the attribute's data: in the stream (direct) or in the array
  // (indexed). Advances `src` past what the stream holds.
  const u8* fetch(const Attr& a, u32 array, const u8*& src, u32 bytes) const {
    if (a.type == DIRECT) {
      const u8* p = src;
      src += bytes;
      return p;
    }
    const u32 index = a.type == INDEX8 ? *src : be16(src);
    src += index_size(a.type);
    const u32 addr = (array_base_[array] + index * array_stride_[array]) & 0x1FFFFFFF;
    return u64{addr} + bytes <= ram_size_ ? ram_ + addr : nullptr;
  }
  void load_normals(const u8*& src, InputVertex& v) const;
  void load_colors(const u8*& src, InputVertex& v) const;

  // The CP words and memory the setup came from: setup() is skipped while
  // they stay the same (most draws).
  u32 key_[5 + 32] = {};
  const u8* key_ram_ = nullptr;
  u32 key_ram_size_ = 0;
  bool keyed_ = false;

  u32 size_ = 0;
  u32 matrix_flags_ = 0;  // VCD bits 0-8: position and texture matrix indices present
  bool normal_index3_ = false;
  Attr pos_, normal_, color_[2], tex_[8];
  u32 pos_bytes_ = 0;
  u32 tex_slots_[8] = {};  // the texture coordinates present, in order
  u32 num_tex_slots_ = 0;
  bool colors_present_ = false;
  u32 array_base_[16] = {}, array_stride_[16] = {};
  const u8* ram_ = nullptr;
  u32 ram_size_ = 0;
  // From begin(): the matrix rows for vertices without matrix indices.
  mutable u8 default_posmtx_ = 0;
  mutable u8 default_texmtx_[8] = {};
};

inline void VertexLoader::load(const u8* src, InputVertex& v) const {
  // Matrix indices: from the vertex if present, else the XF registers (set
  // by begin()).
  if (matrix_flags_) {
    v.posmtx = static_cast<u8>((matrix_flags_ & 1) ? (*src++ & 0x3F) : default_posmtx_);
    for (u32 i = 0; i < 8; ++i)
      v.texmtx[i] = static_cast<u8>((matrix_flags_ & (2u << i)) ? (*src++ & 0x3F) : default_texmtx_[i]);
  }

  // Position. An index of all ones means "skip this vertex".
  v.skip = false;
  if (pos_.type == INDEX8 && *src == 0xFF) v.skip = true;
  if (pos_.type == INDEX16 && be16(src) == 0xFFFF) v.skip = true;
  if (const u8* p = fetch(pos_, ARRAY_POSITION, src, pos_bytes_)) {
    const u32 step = kComponentSize[pos_.format];
    v.pos[0] = component(p, pos_.format, pos_.scale);
    v.pos[1] = component(p + step, pos_.format, pos_.scale);
    v.pos[2] = pos_.count == 3 ? component(p + 2 * step, pos_.format, pos_.scale) : 0.0f;
  } else {
    v.skip = true;
  }

  if (normal_.type != NOT_PRESENT) load_normals(src, v);
  if (colors_present_) load_colors(src, v);

  // Texture coordinates (begin() zeroed the ones the vertices lack).
  for (u32 s = 0; s < num_tex_slots_; ++s) {
    const u32 i = tex_slots_[s];
    const Attr& t = tex_[i];
    const u32 step = kComponentSize[t.format];
    v.tex[i][0] = v.tex[i][1] = 0.0f;
    if (const u8* p = fetch(t, ARRAY_TEX0 + i, src, t.count * step)) {
      v.tex[i][0] = component(p, t.format, t.scale);
      if (t.count == 2) v.tex[i][1] = component(p + step, t.format, t.scale);
    }
  }
}

}  // namespace gx
