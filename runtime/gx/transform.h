// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transform unit (XF): its memory (matrices, lights, registers) and
// the per-vertex work it does. Vertices are transformed on the CPU, so the
// host GPU only rasterises: position and normal transform, projection,
// lighting of the two colour channels, and texture coordinate generation.
//
// The register state is decoded once per draw (prepare()), so the
// per-vertex path only does arithmetic.
//
// Based on the Dolphin software renderer's TransformUnit.cpp (Copyright
// Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "gx/regs.h"
#include "gx/vertex_loader.h"

namespace gx {

struct TransformedVertex {
  float clip[4];     // projected position (GX clip space)
  u32 color[2];      // RGBA8 (red in the lowest byte), as the TEV sees them
  float tex[8][3];   // s, t, q
};

class XfUnit {
 public:
  XfUnit();

  u32 reg(u16 addr) const { return addr < XF_END ? mem_[addr] : 0; }
  float f(u16 addr) const { return addr < XF_END ? fmem_[addr] : 0.0f; }
  void write(u16 addr, u32 value) {
    if (addr >= XF_END) return;
    mem_[addr] = value;
    fmem_[addr] = std::bit_cast<float>(value);
    if (addr >= XF_LIGHTS) dirty_ = true;  // registers and lights are decoded by prepare()
  }

  // Viewport: half width/height (height is negative for the usual y-down
  // EFB), depth range and origin (x/y include the 342 pixel offset).
  float viewport_wd() const { return f(XF_VIEWPORT + 0); }
  float viewport_ht() const { return f(XF_VIEWPORT + 1); }
  float viewport_zrange() const { return f(XF_VIEWPORT + 2); }
  float viewport_x() const { return f(XF_VIEWPORT + 3); }
  float viewport_y() const { return f(XF_VIEWPORT + 4); }
  float viewport_farz() const { return f(XF_VIEWPORT + 5); }
  u32 num_texgens() const { return reg(XF_NUM_TEXGENS) & 0xF; }
  bool perspective() const { return reg(XF_PROJECTION + 6) == 0; }

  // Decodes the registers for the next draws (cheap if nothing changed).
  void prepare();
  // Starts a draw (after prepare()): `out` is the vertex the draw's
  // transform() calls fill. Sets the texture coordinates no generator
  // writes.
  void begin(TransformedVertex& out) const {
    for (u32 i = num_texgens_; i < 8; ++i) out.tex[i][0] = out.tex[i][1] = 0.0f, out.tex[i][2] = 1.0f;
  }
  // Transforms one vertex (after begin()). Defined below, for the draw loop
  // to inline: it runs for every vertex.
  GX_INLINE void transform(const InputVertex& in, TransformedVertex& out) const;

 private:
  struct Channel {
    bool material_from_vertex, lighting, ambient_from_vertex;
    u32 diffuse;      // 0 none, 1 sign, 2 clamp
    u32 attenuation;  // 0 none, 1 specular, 2 directional, 3 spot
    u32 light_mask;
  };
  struct Light {
    float color[4];  // RGBA
    float cosatt[3], distatt[3], pos[3], dir[3];
  };
  struct TexGen {
    bool stq, abc1;
    u32 type;    // 0 regular, 1 emboss, 2 colour 0, 3 colour 1
    u32 source;  // 0 position, 1 normal, 2 colours, 3 tangent, 4 binormal, 5+ texture coordinate n
    u32 emboss_source, emboss_light;
    u32 post_row;  // dual texture transform
    bool post_normalize;
  };

  // A vertex with lighting or normals (the general case, out of line).
  void transform_lit(const InputVertex& in, TransformedVertex& out) const;
  // Texture coordinate generator `i` of the types that need no normals or
  // lights (regular, colour).
  GX_INLINE void texgen(u32 i, const InputVertex& in, const u8 (*colors)[4], TransformedVertex& out) const;

  // XF memory as raw words and as floats (padded: texture matrix rows can
  // reach past the position matrices).
  std::array<u32, XF_END> mem_{};
  std::array<float, XF_END + 16> fmem_{};

  // Decoded by prepare().
  bool dirty_ = true;
  bool perspective_ = true;
  float proj_[6] = {};
  Channel color_[2] = {}, alpha_[2] = {};
  u8 material_[2][4] = {}, ambient_[2][4] = {};
  Light lights_[8] = {};
  bool lighting_ = false, normals_ = false, dual_tex_ = false;
  u32 num_colors_ = 1, num_texgens_ = 0;
  TexGen texgens_[8] = {};
};

GX_INLINE void XfUnit::texgen(u32 i, const InputVertex& in, const u8 (*colors)[4], TransformedVertex& out) const {
  const TexGen& tg = texgens_[i];
  float dx, dy, dz;
  if (tg.type == 0) {  // regular: matrix times a source row
    float sx = 0, sy = 0, sz = 0;
    switch (tg.source) {
      case 0: sx = in.pos[0], sy = in.pos[1], sz = in.pos[2]; break;
      case 1: sx = in.normal[0][0], sy = in.normal[0][1], sz = in.normal[0][2]; break;
      case 3: sx = in.normal[1][0], sy = in.normal[1][1], sz = in.normal[1][2]; break;
      case 4: sx = in.normal[2][0], sy = in.normal[2][1], sz = in.normal[2][2]; break;
      case 2: break;  // colours: only valid for the colour texgen types
      default: {
        const u32 t = tg.source - 5 < 7 ? tg.source - 5 : 7;
        sx = in.tex[t][0], sy = in.tex[t][1], sz = 1.0f;
        break;
      }
    }
    if (std::isnan(sx)) sx = 1.0f;
    if (std::isnan(sy)) sy = 1.0f;
    if (std::isnan(sz)) sz = 1.0f;
    if (!tg.abc1) sz = 1.0f;
    const float* m = &fmem_[XF_POS_MATRICES + in.texmtx[i] * 4];
    dx = m[0] * sx + m[1] * sy + m[2] * sz + m[3];
    dy = m[4] * sx + m[5] * sy + m[6] * sz + m[7];
    dz = tg.stq ? m[8] * sx + m[9] * sy + m[10] * sz + m[11] : 1.0f;
    if (dual_tex_) {
      if (tg.post_normalize) {
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len != 0.0f) dx = dx / len, dy = dy / len, dz = dz / len;
      }
      const float* p = &fmem_[XF_POST_MATRICES + tg.post_row * 4];
      const float px = p[0] * dx + p[1] * dy + p[2] * dz + p[3];
      const float py = p[4] * dx + p[5] * dy + p[6] * dz + p[7];
      const float pz = p[8] * dx + p[9] * dy + p[10] * dz + p[11];
      dx = px, dy = py, dz = pz;
    }
    // q = 0 is a special case on hardware.
    if (dz == 0.0f) {
      dx = std::clamp(dx / 2.0f, -1.0f, 1.0f);
      dy = std::clamp(dy / 2.0f, -1.0f, 1.0f);
    }
  } else {  // colour 0 / colour 1: red and green become s and t
    const u8* c = colors[tg.type & 1];  // types 4-7 are not valid: keep them in bounds
    dx = c[0] / 255.0f, dy = c[1] / 255.0f, dz = 1.0f;
  }
  out.tex[i][0] = dx, out.tex[i][1] = dy, out.tex[i][2] = dz;
}

GX_INLINE void XfUnit::transform(const InputVertex& in, TransformedVertex& out) const {
  if (lighting_ || normals_) {
    transform_lit(in, out);
    return;
  }
  // Position: model-view matrix (3x4), then projection.
  const float* m = &fmem_[XF_POS_MATRICES + in.posmtx * 4];
  const float x = in.pos[0], y = in.pos[1], z = in.pos[2];
  const float vx = m[0] * x + m[1] * y + m[2] * z + m[3];
  const float vy = m[4] * x + m[5] * y + m[6] * z + m[7];
  const float vz = m[8] * x + m[9] * y + m[10] * z + m[11];
  const float* p = proj_;
  if (perspective_) {
    out.clip[0] = p[0] * vx + p[1] * vz;
    out.clip[1] = p[2] * vy + p[3] * vz;
    out.clip[2] = (p[4] * vz + p[5]) * (1.0f - 1e-7f);
    out.clip[3] = -vz;
  } else {
    out.clip[0] = p[0] * vx + p[1];
    out.clip[1] = p[2] * vy + p[3];
    out.clip[2] = p[4] * vz + p[5];
    out.clip[3] = 1.0f;
  }

  // Colours, unlit: from the vertex or the material register.
  u8 colors[2][4];
  for (u32 c = 0; c < 2; ++c) {
    const u8* mat = color_[c].material_from_vertex ? in.color[c].data() : material_[c];
    colors[c][0] = mat[0], colors[c][1] = mat[1], colors[c][2] = mat[2];
    colors[c][3] = alpha_[c].material_from_vertex ? in.color[c][3] : material_[c][3];
  }
  for (u32 i = 0; i < num_texgens_; ++i) texgen(i, in, colors, out);

  auto pack = [](const u8* c) { return c[0] | (u32{c[1]} << 8) | (u32{c[2]} << 16) | (u32{c[3]} << 24); };
  out.color[0] = num_colors_ >= 1 ? pack(colors[0]) : 0;
  out.color[1] = num_colors_ >= 2 ? pack(colors[1]) : 0;
}

}  // namespace gx
