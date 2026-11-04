// SPDX-License-Identifier: GPL-3.0-or-later
//
// The TEV ubershader: one fragment shader for every GX pixel pipeline
// configuration. The BP register state (TEV stages, orders, konst
// selections, indirect texturing, alpha test, fog) arrives as uniforms and
// the shader interprets it per pixel, with the TEV's integer arithmetic.
//
// Ported from Dolphin's UberShaderPixel.cpp and PixelShaderGen.cpp
// (Copyright Dolphin Emulator Project, GPL-2.0-or-later).
#version 450

#ifdef EARLY_Z
// Depth is tested and written before the alpha test (GX "zcomploc").
layout(early_fragment_tests) in;
#endif

layout(location = 0) in vec4 v_color0;
layout(location = 1) in vec4 v_color1;
layout(location = 2) in vec3 v_tex[8];

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D tex0;
layout(set = 2, binding = 1) uniform sampler2D tex1;
layout(set = 2, binding = 2) uniform sampler2D tex2;
layout(set = 2, binding = 3) uniform sampler2D tex3;
layout(set = 2, binding = 4) uniform sampler2D tex4;
layout(set = 2, binding = 5) uniform sampler2D tex5;
layout(set = 2, binding = 6) uniform sampler2D tex6;
layout(set = 2, binding = 7) uniform sampler2D tex7;

// Must match gx::PixelUniforms (runtime/gx/frame.h).
layout(std140, set = 3, binding = 0) uniform PixelBlock {
  ivec4 colors[4];     // TEV registers PREV, C0, C1, C2
  ivec4 konst[32];     // konst colour table
  ivec4 alpha_ref;
  ivec4 texsize[8];    // per texture map
  ivec4 tcscale[8];    // per texture coordinate
  ivec4 indscale[2];
  ivec4 indmtx[6];
  ivec4 fog_color;
  ivec4 fog_i;
  vec4 fog_f;
  vec4 fog_range[3];
  uvec4 stages[16];    // colour combiner, alpha combiner, indirect command, iref (per indirect stage)
  uvec4 orders[8];     // TEV order, konst/swap select
  uvec4 misc;          // genmode, alpha test, fog param 3, fog range base
  uvec4 misc2;         // dst alpha, rgba6, dither, logic op
};

// Set by the renderer.
layout(std140, set = 3, binding = 1) uniform HostBlock {
  vec4 host;  // x: EFB scale (host pixels per EFB pixel), y: widescreen side (EFB pixels)
};

// Per-pixel TEV state.
ivec4 g_reg[4];
ivec4 g_tex;
ivec4 g_ras;
ivec4 g_konst;
int g_alpha_bump;

int idot(ivec3 a, ivec3 b) {
  ivec3 t = a * b;
  return t.x + t.y + t.z;
}

// Samples texture map `map` at fixed-point texel coordinates (1/128 texel).
ivec4 sample_tex(uint map, ivec2 uv) {
  vec2 coords = vec2(uv) / (vec2(texsize[map].xy) * 128.0);
  vec4 c;
  switch (map) {
    case 0u: c = texture(tex0, coords); break;
    case 1u: c = texture(tex1, coords); break;
    case 2u: c = texture(tex2, coords); break;
    case 3u: c = texture(tex3, coords); break;
    case 4u: c = texture(tex4, coords); break;
    case 5u: c = texture(tex5, coords); break;
    case 6u: c = texture(tex6, coords); break;
    default: c = texture(tex7, coords); break;
  }
  return ivec4(round(c * 255.0));
}

// Colour channel swap table `s`.
ivec4 swizzle(uint s, ivec4 c) {
  uint a = orders[s * 2u].y, b = orders[s * 2u + 1u].y;
  return ivec4(c[bitfieldExtract(a, 0, 2)], c[bitfieldExtract(a, 2, 2)], c[bitfieldExtract(b, 0, 2)],
               c[bitfieldExtract(b, 2, 2)]);
}

int wrap(int coord, uint mode) {
  if (mode == 0u) return coord;
  if (mode < 6u) return coord & (0xfffe >> mode);
  return 0;
}

// The TEV's linear interpolation, with bias, add/subtract and scale.
ivec3 tev_lerp3(ivec3 A, ivec3 B, ivec3 C, ivec3 D, uint bias, bool op, uint scale) {
  C += C >> 7;  // 0..255 -> 0..256
  if (bias == 1u) D += 128;
  else if (bias == 2u) D -= 128;
  ivec3 lerp = (A << 8) + (B - A) * C;
  if (scale != 3u) {
    lerp = lerp << scale;
    D = D << scale;
    lerp = lerp + (op ? 127 : 128);  // rounding (not applied when dividing by 2)
  }
  ivec3 result = lerp >> 8;
  result = op ? D - result : D + result;
  if (scale == 3u) result = result >> 1;
  return result;
}

int tev_lerp(int A, int B, int C, int D, uint bias, bool op, uint scale) {
  return tev_lerp3(ivec3(A), ivec3(B), ivec3(C), ivec3(D), bias, op, scale).x;
}

// Compare modes 0-5 (shared by the colour and alpha combiners).
bool tev_compare(uint op, ivec3 a, ivec3 b) {
  switch (op) {
    case 0u: return a.r > b.r;
    case 1u: return a.r == b.r;
    case 2u: return (a.r | (a.g << 8)) > (b.r | (b.g << 8));
    case 3u: return a.r == b.r && a.g == b.g;
    case 4u: return (a.r | (a.g << 8) | (a.b << 16)) > (b.r | (b.g << 8) | (b.b << 16));
    case 5u: return a.r == b.r && a.g == b.g && a.b == b.b;
    default: return false;
  }
}

ivec3 color_input(uint index) {
  switch (index) {
    case 0u: return g_reg[0].rgb;
    case 1u: return g_reg[0].aaa;
    case 2u: return g_reg[1].rgb;
    case 3u: return g_reg[1].aaa;
    case 4u: return g_reg[2].rgb;
    case 5u: return g_reg[2].aaa;
    case 6u: return g_reg[3].rgb;
    case 7u: return g_reg[3].aaa;
    case 8u: return g_tex.rgb;
    case 9u: return g_tex.aaa;
    case 10u: return g_ras.rgb;
    case 11u: return g_ras.aaa;
    case 12u: return ivec3(255);
    case 13u: return ivec3(128);
    case 14u: return g_konst.rgb;
    default: return ivec3(0);
  }
}

int alpha_input(uint index) {
  switch (index) {
    case 0u: return g_reg[0].a;
    case 1u: return g_reg[1].a;
    case 2u: return g_reg[2].a;
    case 3u: return g_reg[3].a;
    case 4u: return g_tex.a;
    case 5u: return g_ras.a;
    case 6u: return g_konst.a;
    default: return 0;
  }
}

bool alpha_compare(int a, int b, uint mode) {
  switch (mode) {
    case 0u: return false;
    case 1u: return a < b;
    case 2u: return a == b;
    case 3u: return a <= b;
    case 4u: return a > b;
    case 5u: return a != b;
    case 6u: return a >= b;
    default: return true;
  }
}

void main() {
  uint genmode = misc.x;
  uint num_stages = bitfieldExtract(genmode, 10, 4);  // stages - 1
  uint num_texgens = bitfieldExtract(genmode, 0, 4);

  for (int i = 0; i < 4; ++i) g_reg[i] = colors[i];
  g_tex = ivec4(0);
  g_alpha_bump = 0;
  ivec4 raw_tex = ivec4(0);
  ivec3 tevcoord = ivec3(0);

  // Texture coordinates in fixed point (1/128 texel).
  // Only the coordinates the draw has (all the reads below are of those).
  ivec2 uv[8] = ivec2[8](ivec2(0), ivec2(0), ivec2(0), ivec2(0), ivec2(0), ivec2(0), ivec2(0), ivec2(0));
  for (int i = 0; i < int(min(num_texgens, 8u)); ++i) {
    vec3 t = v_tex[i];
    vec2 st = t.z == 0.0 ? t.xy : t.xy / t.z;
    uv[i] = ivec2(st * vec2(tcscale[i].xy) * 128.0);
  }

  for (uint stage = 0u; stage <= num_stages; ++stage) {
    uint cc = stages[stage].x;
    uint ac = stages[stage].y;
    uint order = orders[stage >> 1].x;
    if ((stage & 1u) == 1u) order >>= 12;

    // ---- Texturing (with indirect coordinates) ----
    if (num_texgens != 0u) {
      uint tc = bitfieldExtract(order, 3, 3);
      ivec2 fixed_uv = uv[tc < num_texgens ? tc : 0u];
      uint tevind = stages[stage].z;
      if (tevind != 0u) {
        uint bt = bitfieldExtract(tevind, 0, 2);
        uint fmt = bitfieldExtract(tevind, 2, 2);
        uint bias = bitfieldExtract(tevind, 4, 3);
        uint bs = bitfieldExtract(tevind, 7, 2);
        uint matrix_index = bitfieldExtract(tevind, 9, 2);
        uint matrix_id = bitfieldExtract(tevind, 11, 2);
        ivec2 indtevtrans = ivec2(0);
        uint iref = stages[bt].w;
        if (iref != 0u) {
          uint itc = bitfieldExtract(iref, 0, 3);
          ivec2 iuv = uv[itc < num_texgens ? itc : 0u];
          ivec4 sc = indscale[bt >> 1];
          iuv = (bt & 1u) == 0u ? (iuv >> sc.xy) : (iuv >> sc.zw);
          ivec3 indcoord = sample_tex(bitfieldExtract(iref, 8, 3), iuv).abg;
          if (bs != 0u) g_alpha_bump = indcoord[bs - 1u];
          switch (fmt) {
            case 0u:
              indcoord.x += (bias & 1u) != 0u ? -128 : 0;
              indcoord.y += (bias & 2u) != 0u ? -128 : 0;
              indcoord.z += (bias & 4u) != 0u ? -128 : 0;
              g_alpha_bump = g_alpha_bump & 0xf8;
              break;
            case 1u:
              indcoord = (indcoord >> 3) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
              g_alpha_bump = g_alpha_bump << 5;
              break;
            case 2u:
              indcoord = (indcoord >> 4) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
              g_alpha_bump = g_alpha_bump << 4;
              break;
            default:
              indcoord = (indcoord >> 5) + ivec3((bias & 1u) != 0u ? 1 : 0, (bias & 2u) != 0u ? 1 : 0, (bias & 4u) != 0u ? 1 : 0);
              g_alpha_bump = g_alpha_bump << 3;
              break;
          }
          if (matrix_index != 0u) {
            uint m = 2u * (matrix_index - 1u);
            int shift = indmtx[m].w;
            if (matrix_id == 0u)
              indtevtrans = ivec2(idot(indmtx[m].xyz, indcoord), idot(indmtx[m + 1u].xyz, indcoord)) >> 3;
            else if (matrix_id == 1u)
              indtevtrans = (fixed_uv * indcoord.xx) >> 8;
            else if (matrix_id == 2u)
              indtevtrans = (fixed_uv * indcoord.yy) >> 8;
            if (shift >= 0) indtevtrans = indtevtrans >> shift;
            else indtevtrans = indtevtrans << ((-shift) & 31);
          }
        }
        ivec2 wrapped = ivec2(wrap(fixed_uv.x, bitfieldExtract(tevind, 13, 3)), wrap(fixed_uv.y, bitfieldExtract(tevind, 16, 3)));
        if ((tevind & (1u << 20)) != 0u) tevcoord.xy += wrapped + indtevtrans;
        else tevcoord.xy = wrapped + indtevtrans;
        tevcoord.xy = (tevcoord.xy << 8) >> 8;  // s24 overflow
      } else {
        tevcoord.xy = fixed_uv;
      }

      if ((order & 64u) != 0u) {
        raw_tex = sample_tex(bitfieldExtract(order, 0, 3), tevcoord.xy);
        g_tex = swizzle(bitfieldExtract(ac, 2, 2), raw_tex);
      } else {
        g_tex = ivec4(255);
      }
    }

    // ---- Rasterised colour and konst for this stage ----
    uint ras = bitfieldExtract(order, 7, 3);
    if (ras < 2u) {
      g_ras = swizzle(bitfieldExtract(ac, 0, 2), ivec4(round((ras == 0u ? v_color0 : v_color1) * 255.0)));
    } else if (ras == 5u) {
      g_ras = ivec4(g_alpha_bump);
    } else if (ras == 6u) {
      g_ras = ivec4(g_alpha_bump | (g_alpha_bump >> 5));
    } else {
      g_ras = ivec4(0);
    }
    uint ksel = orders[stage >> 1].y;
    uint kc = (stage & 1u) == 0u ? bitfieldExtract(ksel, 4, 5) : bitfieldExtract(ksel, 14, 5);
    uint ka = (stage & 1u) == 0u ? bitfieldExtract(ksel, 9, 5) : bitfieldExtract(ksel, 19, 5);
    g_konst = ivec4(konst[kc].rgb, konst[ka].a);

    // ---- Colour combiner ----
    uint c_bias = bitfieldExtract(cc, 16, 2);
    bool c_op = bitfieldExtract(cc, 18, 1) != 0u;
    uint c_scale = bitfieldExtract(cc, 20, 2);
    uint c_cmp = (c_scale << 1) | uint(c_op);
    ivec3 cA = color_input(bitfieldExtract(cc, 12, 4)) & ivec3(255);
    ivec3 cB = color_input(bitfieldExtract(cc, 8, 4)) & ivec3(255);
    ivec3 cC = color_input(bitfieldExtract(cc, 4, 4)) & ivec3(255);
    ivec3 cD = color_input(bitfieldExtract(cc, 0, 4));
    ivec3 color;
    if (c_bias != 3u) {
      color = tev_lerp3(cA, cB, cC, cD, c_bias, c_op, c_scale);
    } else {
      if (c_cmp == 6u) {
        color = ivec3(cA.r > cB.r ? cC.r : 0, cA.g > cB.g ? cC.g : 0, cA.b > cB.b ? cC.b : 0);
      } else if (c_cmp == 7u) {
        color = ivec3(cA.r == cB.r ? cC.r : 0, cA.g == cB.g ? cC.g : 0, cA.b == cB.b ? cC.b : 0);
      } else {
        color = tev_compare(c_cmp, cA, cB) ? cC : ivec3(0);
      }
      color = cD + color;
    }
    color = bitfieldExtract(cc, 19, 1) != 0u ? clamp(color, 0, 255) : clamp(color, -1024, 1023);

    // ---- Alpha combiner ----
    uint a_bias = bitfieldExtract(ac, 16, 2);
    bool a_op = bitfieldExtract(ac, 18, 1) != 0u;
    uint a_scale = bitfieldExtract(ac, 20, 2);
    uint a_cmp = (a_scale << 1) | uint(a_op);
    int aA = alpha_input(bitfieldExtract(ac, 13, 3)) & 255;
    int aB = alpha_input(bitfieldExtract(ac, 10, 3)) & 255;
    int aC = alpha_input(bitfieldExtract(ac, 7, 3)) & 255;
    int aD = alpha_input(bitfieldExtract(ac, 4, 3));
    int alpha;
    if (a_bias != 3u) {
      alpha = tev_lerp(aA, aB, aC, aD, a_bias, a_op, a_scale);
    } else {
      if (a_cmp == 6u) alpha = aA > aB ? aC : 0;
      else if (a_cmp == 7u) alpha = aA == aB ? aC : 0;
      else alpha = tev_compare(a_cmp, cA, cB) ? aC : 0;  // these compare the colour inputs
      alpha = aD + alpha;
    }
    alpha = bitfieldExtract(ac, 19, 1) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);

    g_reg[bitfieldExtract(cc, 22, 2)].rgb = color;
    g_reg[bitfieldExtract(ac, 22, 2)].a = alpha;
  }

  ivec4 result;
  result.rgb = g_reg[bitfieldExtract(stages[num_stages].x, 22, 2)].rgb;
  result.a = g_reg[bitfieldExtract(stages[num_stages].y, 22, 2)].a;
  result &= 255;

  // ---- Alpha test ----
  uint at = misc.y;
  if (at != 0u) {
    bool c0 = alpha_compare(result.a, alpha_ref.x, bitfieldExtract(at, 16, 3));
    bool c1 = alpha_compare(result.a, alpha_ref.y, bitfieldExtract(at, 19, 3));
    bool pass;
    switch (bitfieldExtract(at, 22, 2)) {
      case 0u: pass = c0 && c1; break;
      case 1u: pass = c0 || c1; break;
      case 2u: pass = c0 != c1; break;
      default: pass = c0 == c1; break;
    }
    if (!pass) discard;
  }
  // An alpha of 1 passes the alpha test but acts as 0 in blending.
  if (result.a == 1) result.a = 0;

  // ---- Fog ----
  uint fog_param3 = misc.z;
  uint fog_fn = bitfieldExtract(fog_param3, 21, 3);
  if (fog_fn != 0u) {
    int zcoord = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xFFFFFF);
    float ze;
    if (bitfieldExtract(fog_param3, 20, 1) == 0u) {
      ze = (fog_f.x * 16777216.0) / float(fog_i.y - (zcoord >> fog_i.w));  // perspective
    } else {
      ze = fog_f.x * float(zcoord) / 16777216.0;  // orthographic
    }
    if (bitfieldExtract(misc.w, 10, 1) != 0u) {  // range adjustment
      float offset = (2.0 * ((gl_FragCoord.x / host.x - host.y) / fog_f.w)) - 1.0 - fog_f.z;
      float findex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
      uint lo = uint(findex), hi = lo + 1u;
      float k = mix(fog_range[lo >> 2u][lo & 3u], fog_range[hi >> 2u][hi & 3u], fract(findex));
      ze *= sqrt(offset * offset + k * k) / k;
    }
    float fog = clamp(ze - fog_f.y, 0.0, 1.0);
    switch (fog_fn) {
      case 4u: fog = 1.0 - exp2(-8.0 * fog); break;
      case 5u: fog = 1.0 - exp2(-8.0 * fog * fog); break;
      case 6u: fog = exp2(-8.0 * (1.0 - fog)); break;
      case 7u: fog = 1.0 - fog; fog = exp2(-8.0 * fog * fog); break;
      default: break;
    }
    int ifog = int(round(fog * 256.0));
    result.rgb = (result.rgb * (256 - ifog) + fog_color.rgb * ifog) >> 8;
  }

  // ---- Logic ops (approximated; only the ones that ignore the framebuffer) ----
  uint logic = misc2.w;
  if ((logic & 16u) != 0u) {
    switch (logic & 15u) {
      case 0u: result = ivec4(0); break;              // clear
      case 12u: result ^= ivec4(255); break;          // copy inverted
      case 10u: case 15u: result = ivec4(255); break;  // invert, set
      default: break;
    }
  }

  out_color.rgb = vec3(result.rgb) / 255.0;
  uint dst_alpha = misc2.x;
  out_color.a = dst_alpha != 0u ? float(bitfieldExtract(dst_alpha, 0, 8)) / 255.0 : float(result.a) / 255.0;
}
