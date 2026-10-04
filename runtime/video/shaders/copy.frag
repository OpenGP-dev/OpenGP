// SPDX-License-Identifier: GPL-3.0-or-later
//
// EFB copy: reads the EFB (colour or depth), applies the vertical copy
// filter, gamma and RGB->YUV conversion, and writes what the game would
// read back as a texture of the copy's format (decoded to RGBA8).
//
// Ported from Dolphin's TextureConverterShaderGen.cpp (Copyright Dolphin
// Emulator Project, GPL-2.0-or-later).
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D efb;

layout(std140, set = 3, binding = 0) uniform CopyBlock {
  vec4 pixel;    // x: height of one EFB row in uv, y/z: clamp top/bottom (uv), w: 1 / gamma
  uvec4 params;  // x: format, y: flags (1 depth, 2 intensity, 4 EFB has alpha, 8 gamma)
  uvec4 filt;    // copy filter row weights (above, current, below)
};

uvec4 sample_efb(float row) {
  vec4 t = texture(efb, vec2(v_uv.x, clamp(v_uv.y + row * pixel.x, pixel.y, pixel.z)));
  if ((params.y & 1u) != 0u) {
    uint depth = uint(clamp(t.x, 0.0, 1.0) * 16777215.0);
    return uvec4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);
  }
  return uvec4(round(t * 255.0));
}

void main() {
  uvec4 cur = sample_efb(0.0);
  uvec3 rows = cur.rgb * filt.y;
  if (filt.x != 0u || filt.z != 0u) rows += sample_efb(-1.0).rgb * filt.x + sample_efb(1.0).rgb * filt.z;
  uvec4 c = uvec4(rows >> 6, (params.y & 4u) != 0u ? cur.a : 255u);
  c = min(c, uvec4(255u));

  if ((params.y & 8u) != 0u)
    c = uvec4(round(pow(vec4(c) / 255.0, vec4(pixel.w, pixel.w, pixel.w, 1.0)) * 255.0));

  if ((params.y & 2u) != 0u) {
    // RGB -> YUV (constants from hardware tests)
    vec4 rgb1 = vec4(vec3(c.rgb), 256.0);
    uvec3 yuv = uvec3(dot(vec4(66, 129, 25, 16), rgb1), dot(vec4(-38, -74, 112, 128), rgb1),
                      dot(vec4(112, -94, -18, 128), rgb1));
    c.rgb = (yuv >> 8) + ((yuv >> 7) & 1u);
  }

  switch (params.x) {
    case 0u: {  // R4
      float r = float(c.r & 0xF0u) / 240.0;
      out_color = vec4(r);
      break;
    }
    case 1u:  // R8
    case 8u: out_color = vec4(c.rrrr) / 255.0; break;
    case 2u: {  // RA4
      vec2 ra = vec2(c.ra & 0xF0u) / 240.0;
      out_color = ra.xxxy;
      break;
    }
    case 3u: out_color = vec4(c.rrra) / 255.0; break;  // RA8
    case 4u: {  // RGB565
      vec2 rb = vec2(c.rb & 0xF8u) / 248.0;
      out_color = vec4(rb.x, float(c.g & 0xFCu) / 252.0, rb.y, 1.0);
      break;
    }
    case 5u:  // RGB5A3
      out_color = vec4(vec3(c.rgb & 0xF8u) / 248.0, float(c.a & 0xE0u) / 224.0);
      break;
    case 7u: out_color = vec4(c.aaaa) / 255.0; break;   // A8
    case 9u: out_color = vec4(c.gggg) / 255.0; break;   // G8
    case 10u: out_color = vec4(c.bbbb) / 255.0; break;  // B8
    case 11u: out_color = vec4(c.rrrg) / 255.0; break;  // RG8
    case 12u: out_color = vec4(c.gggb) / 255.0; break;  // GB8
    case 15u: out_color = vec4(vec3(c.rgb) / 255.0, 1.0); break;  // XFB
    default: out_color = vec4(c) / 255.0; break;        // RGBA8
  }
}
