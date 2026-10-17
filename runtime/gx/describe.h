// SPDX-License-Identifier: GPL-3.0-or-later
//
// GX state in words, for frame captures and profiles: formats, primitives,
// the pipeline key (blending, depth) and the TEV stages as formulas.
#pragma once

#include <string>

#include "gx/frame.h"

namespace gx {

const char* texture_format_name(u32 format);  // TexFormat value: "I4", "CMPR"...
const char* copy_format_name(CopyFormat format);
const char* pixel_format_name(u32 format);     // BP_ZCOMPARE bits 0-2: "RGB8_Z24"...
const char* primitive_name(u32 primitive);     // 0-7: "quads", "triangles"...

// A PipelineKey's blending and depth: "blend src alpha,1-src alpha, colour writes, depth lequal + write".
std::string describe_pipeline(u32 key);

// A texture unit's sampling (TX_SETMODE0/1): "wrap repeat,clamp, linear/linear, mip linear lod 0-4 bias 0".
std::string describe_sampler(u32 mode0, u32 mode1);

// The TEV stages of a draw, one line each:
//   "s0: tex0[uv0] ras0 | rgb = ras.rgb*tex.rgb | a = tex.a*ras.a"
// then the alpha test.
std::string describe_tev(const PixelUniforms& u);

}  // namespace gx
