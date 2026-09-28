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

#include <array>
#include <bit>

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

  // Decodes the registers for the next draws (cheap if nothing changed).
  void prepare();
  // Transforms one vertex (after prepare()).
  void transform(const InputVertex& in, TransformedVertex& out) const;

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

}  // namespace gx
