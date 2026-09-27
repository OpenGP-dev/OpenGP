// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transform unit (XF): its memory (matrices, lights, registers) and
// the per-vertex work it does. Vertices are transformed on the CPU, so the
// host GPU only rasterises: position and normal transform, projection,
// lighting of the two colour channels, and texture coordinate generation.
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
  float view[3];     // position after the model-view matrix
  float normal[3][3];
  std::array<u8, 4> color[2];  // RGBA
  float tex[8][3];   // s, t, q
};

class XfUnit {
 public:
  XfUnit();

  u32 reg(u16 addr) const { return addr < XF_END ? mem_[addr] : 0; }
  float f(u16 addr) const { return std::bit_cast<float>(reg(addr)); }
  void write(u16 addr, u32 value) {
    if (addr < XF_END) mem_[addr] = value;
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

  void transform(const InputVertex& in, TransformedVertex& out) const;

 private:
  const float* row(u16 addr) const { return reinterpret_cast<const float*>(&mem_[addr]); }
  void transform_color(const InputVertex& in, TransformedVertex& out) const;
  void transform_texcoords(const InputVertex& in, TransformedVertex& out) const;

  // XF memory as raw words (floats are stored by value, host endian).
  std::array<u32, XF_END> mem_{};
};

}  // namespace gx
