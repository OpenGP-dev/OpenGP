// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vertex loader: turns the vertices of a GX primitive (in the format the
// CP vertex descriptor and attribute table describe) into InputVertex,
// resolving indexed attributes through the CP arrays in guest memory.
//
// Based on Dolphin's VertexLoader*.cpp and the software renderer's
// SWVertexLoader.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <array>

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
  // from `ram` (guest MEM1, `ram_size` bytes).
  void setup(const u32* cp, u8 vat, const u8* ram, u32 ram_size);

  // Bytes per vertex in the command stream.
  u32 size() const { return size_; }

  // Decodes one vertex from `src`. `matrix_index_a/b` are the XF matrix
  // index registers, used when the vertex has no matrix indices.
  // Attributes the vertex lacks keep their previous values in `v` (normals),
  // or get defaults (colours: white).
  void load(const u8* src, InputVertex& v, u32 matrix_index_a, u32 matrix_index_b) const;

  bool has_normal() const { return normal_.type != 0; }

 private:
  struct Attr {
    u32 type = 0;     // 0 not present, 1 direct, 2 index8, 3 index16
    u32 format = 0;   // component or colour format
    u32 count = 0;    // components
    float scale = 1;  // dequantisation for integer formats
  };

  const u8* fetch(const Attr& a, u32 array, const u8*& src, u32 bytes) const;

  u32 size_ = 0;
  u32 matrix_flags_ = 0;  // VCD bits 0-8: position and texture matrix indices present
  bool normal_index3_ = false;
  Attr pos_, normal_, color_[2], tex_[8];
  u32 array_base_[16] = {}, array_stride_[16] = {};
  const u8* ram_ = nullptr;
  u32 ram_size_ = 0;
};

}  // namespace gx
