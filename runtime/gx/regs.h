// SPDX-License-Identifier: GPL-3.0-or-later
//
// GX register addresses and field layouts: BP (the blitting processor,
// rasteriser, texture and pixel pipeline), XF (the transform unit) and the
// texture and EFB copy formats.
//
// Layouts follow Dolphin's BPMemory.h and XFMemory.h (Copyright Dolphin
// Emulator Project, GPL-2.0-or-later).
#pragma once

#include "gekko/types.h"

namespace gx {

using namespace gekko;

// Embedded framebuffer size (pixels).
inline constexpr u32 kEfbWidth = 640;
inline constexpr u32 kEfbHeight = 528;

constexpr u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }
constexpr s32 sbits(u32 v, u32 shift, u32 count) {
  return static_cast<s32>(v << (32 - shift - count)) >> (32 - count);
}

// ---- BP registers ----------------------------------------------------------------------------
enum : u8 {
  BP_GENMODE = 0x00,
  BP_DISPLAY_COPY_FILTER = 0x01,  // 0x01-0x04 (antialiasing sample pattern)
  BP_IND_MTXA = 0x06,             // 0x06 + 3 * matrix
  BP_IND_MTXB = 0x07,
  BP_IND_MTXC = 0x08,
  BP_IND_IMASK = 0x0F,
  BP_IND_CMD = 0x10,  // 0x10-0x1F, one per TEV stage
  BP_SCISSOR_TL = 0x20,
  BP_SCISSOR_BR = 0x21,
  BP_LINE_POINT_WIDTH = 0x22,
  BP_RAS1_SS0 = 0x25,  // indirect texture coordinate scales, stages 0-1
  BP_RAS1_SS1 = 0x26,  // stages 2-3
  BP_IREF = 0x27,
  BP_TREF = 0x28,     // 0x28-0x2F: TEV stage orders, two stages each
  BP_SU_SSIZE = 0x30,  // 0x30 + 2 * texcoord
  BP_SU_TSIZE = 0x31,
  BP_ZMODE = 0x40,
  BP_BLENDMODE = 0x41,
  BP_CONSTANT_ALPHA = 0x42,
  BP_ZCOMPARE = 0x43,  // PE control: EFB pixel format, early depth test
  BP_FIELD_MASK = 0x44,
  BP_SET_DRAW_DONE = 0x45,
  BP_PE_TOKEN = 0x47,
  BP_PE_TOKEN_INT = 0x48,
  BP_EFB_TL = 0x49,
  BP_EFB_WH = 0x4A,
  BP_EFB_ADDR = 0x4B,
  BP_EFB_STRIDE = 0x4D,
  BP_COPY_Y_SCALE = 0x4E,
  BP_CLEAR_AR = 0x4F,
  BP_CLEAR_GB = 0x50,
  BP_CLEAR_Z = 0x51,
  BP_TRIGGER_EFB_COPY = 0x52,
  BP_COPY_FILTER0 = 0x53,
  BP_COPY_FILTER1 = 0x54,
  BP_SCISSOR_OFFSET = 0x59,
  BP_PRELOAD_ADDR = 0x60,
  BP_PRELOAD_TMEM_EVEN = 0x61,
  BP_PRELOAD_TMEM_ODD = 0x62,
  BP_PRELOAD_MODE = 0x63,
  BP_LOAD_TLUT0 = 0x64,
  BP_LOAD_TLUT1 = 0x65,
  BP_TEX_INVALIDATE = 0x66,
  BP_TX_SETMODE0 = 0x80,  // texture units 0-3; units 4-7 at +0x20
  BP_TX_SETMODE1 = 0x84,
  BP_TX_SETIMAGE0 = 0x88,
  BP_TX_SETIMAGE1 = 0x8C,
  BP_TX_SETIMAGE2 = 0x90,
  BP_TX_SETIMAGE3 = 0x94,
  BP_TX_SETTLUT = 0x98,
  BP_TEV_COLOR_ENV = 0xC0,  // 0xC0 + 2 * stage
  BP_TEV_ALPHA_ENV = 0xC1,
  BP_TEV_REGISTER_L = 0xE0,  // 0xE0 + 2 * register: red and alpha
  BP_TEV_REGISTER_H = 0xE1,  // blue and green
  BP_FOG_RANGE = 0xE8,       // 0xE8-0xED
  BP_FOG_PARAM0 = 0xEE,
  BP_FOG_B_MAGNITUDE = 0xEF,
  BP_FOG_B_EXPONENT = 0xF0,
  BP_FOG_PARAM3 = 0xF1,
  BP_FOG_COLOR = 0xF2,
  BP_ALPHA_COMPARE = 0xF3,
  BP_ZTEX_BIAS = 0xF4,
  BP_ZTEX2 = 0xF5,
  BP_TEV_KSEL = 0xF6,  // 0xF6-0xFD
  BP_MASK = 0xFE,
};

// BP register of texture unit `unit` (0-7) for a unit-0 register `reg`.
constexpr u8 tex_reg(u8 reg, u32 unit) { return static_cast<u8>(reg + (unit & 3) + ((unit & 4) << 3)); }

// GENMODE fields
constexpr u32 genmode_num_texgens(u32 v) { return bits(v, 0, 4); }
constexpr u32 genmode_num_tev_stages(u32 v) { return bits(v, 10, 4) + 1; }
constexpr u32 genmode_cull_mode(u32 v) { return bits(v, 14, 2); }  // 0 none, 1 back, 2 front, 3 all
constexpr u32 genmode_num_ind_stages(u32 v) { return bits(v, 16, 3); }

// EFB pixel formats (BP_ZCOMPARE bits 0-2).
enum class PixelFormat : u32 { RGB8_Z24 = 0, RGBA6_Z24 = 1, RGB565_Z16 = 2, Z24 = 3, Y8 = 4, U8 = 5, V8 = 6, YUV420 = 7 };

// Texture formats (TX_SETIMAGE0 bits 20-23).
enum class TexFormat : u32 {
  I4 = 0x0,
  I8 = 0x1,
  IA4 = 0x2,
  IA8 = 0x3,
  RGB565 = 0x4,
  RGB5A3 = 0x5,
  RGBA8 = 0x6,
  C4 = 0x8,
  C8 = 0x9,
  C14X2 = 0xA,
  CMPR = 0xE,
};
enum class TlutFormat : u32 { IA8 = 0, RGB565 = 1, RGB5A3 = 2 };

// EFB copy target formats (the copy trigger's format field, rearranged: see
// copy_format()).
enum class CopyFormat : u32 {
  R4 = 0x0,
  R8_1 = 0x1,
  RA4 = 0x2,
  RA8 = 0x3,
  RGB565 = 0x4,
  RGB5A3 = 0x5,
  RGBA8 = 0x6,
  A8 = 0x7,
  R8 = 0x8,
  G8 = 0x9,
  B8 = 0xA,
  RG8 = 0xB,
  GB8 = 0xC,
  XFB = 0xF,  // not a GX value: a copy to the external framebuffer
};

// EFB copy trigger (BP_TRIGGER_EFB_COPY) fields
constexpr bool copy_clamp_top(u32 v) { return bits(v, 0, 1); }
constexpr bool copy_clamp_bottom(u32 v) { return bits(v, 1, 1); }
constexpr CopyFormat copy_format(u32 v) {
  const u32 f = bits(v, 3, 4);
  return static_cast<CopyFormat>(f / 2 + (f & 1) * 8);
}
constexpr u32 copy_gamma(u32 v) { return bits(v, 7, 2); }
constexpr bool copy_half_scale(u32 v) { return bits(v, 9, 1); }
constexpr bool copy_scale_invert(u32 v) { return bits(v, 10, 1); }
constexpr bool copy_clear(u32 v) { return bits(v, 11, 1); }
constexpr bool copy_to_xfb(u32 v) { return bits(v, 14, 1); }
constexpr bool copy_intensity(u32 v) { return bits(v, 15, 1); }
constexpr bool copy_auto_conv(u32 v) { return bits(v, 16, 1); }

// ---- XF memory -------------------------------------------------------------------------------
enum : u16 {
  XF_POS_MATRICES = 0x000,     // 64 rows of 4 floats (3x4 matrices)
  XF_NORMAL_MATRICES = 0x400,  // 32 rows of 3 floats
  XF_POST_MATRICES = 0x500,    // 64 rows of 4 floats
  XF_LIGHTS = 0x600,           // 8 lights of 16 words
  XF_REGISTERS = 0x1000,
  XF_CLIP_DISABLE = 0x1005,
  XF_VTX_SPECS = 0x1008,
  XF_NUM_COLORS = 0x1009,
  XF_AMBIENT0 = 0x100A,
  XF_MATERIAL0 = 0x100C,
  XF_COLOR0_CTRL = 0x100E,
  XF_ALPHA0_CTRL = 0x1010,
  XF_DUAL_TEX = 0x1012,
  XF_MATRIX_INDEX_A = 0x1018,
  XF_MATRIX_INDEX_B = 0x1019,
  XF_VIEWPORT = 0x101A,    // 6 floats: wd, ht, zRange, xOrig, yOrig, farZ
  XF_PROJECTION = 0x1020,  // 6 floats, then the type (0 perspective, 1 orthographic)
  XF_NUM_TEXGENS = 0x103F,
  XF_TEXGEN = 0x1040,    // 8 words
  XF_POST_TEXGEN = 0x1050,  // 8 words
  XF_END = 0x1058,
};

}  // namespace gx
