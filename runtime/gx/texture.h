// SPDX-License-Identifier: GPL-3.0-or-later
//
// GX texture formats: sizes in memory and decoding to RGBA8.
//
// Textures are stored in tiles (blocks) of 32 bytes; each format has its
// own block size in texels. Mip levels follow the base level in memory,
// each padded to whole blocks. Paletted formats (C4, C8, C14X2) look their
// colours up in a TLUT loaded into texture memory (TMEM).
//
// Decoders ported from Dolphin's TextureDecoder_Generic.cpp,
// TextureDecoder_Util.h and LookUpTables.h (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include <span>

#include "gx/regs.h"

namespace gx {

// Size of texture memory, where TLUTs (and preloaded textures) live.
inline constexpr u32 kTmemSize = 1024 * 1024;

u32 block_width(TexFormat format);
u32 block_height(TexFormat format);
// Bytes of one mip level of `width` x `height` texels.
u32 texture_level_size(TexFormat format, u32 width, u32 height);
// Bytes of all `levels` mip levels.
u32 texture_size(TexFormat format, u32 width, u32 height, u32 levels);
bool is_paletted(TexFormat format);
// Number of palette entries a paletted format can index.
u32 palette_entries(TexFormat format);
bool is_valid_format(u32 format);

// Decodes one level into `dst` (width * height RGBA8 pixels, red in the
// lowest byte). `tlut` holds the palette (big-endian 16-bit entries) for
// paletted formats. `scratch` needs room for the block-padded level.
void decode_texture(u32* dst, const u8* src, TexFormat format, u32 width, u32 height, const u8* tlut,
                    TlutFormat tlut_format, std::span<u32> scratch);

// The format of the texture an EFB copy produces, when read back as a texture.
TexFormat copy_texture_format(CopyFormat format);

// Whether a texture's mipmaps make it fade in with distance: level 0 (nearly)
// all transparent and a smaller level visible, as GP 2's rainbows are. Such
// levels are an effect, not downscaled copies, so they are chosen as at the
// game's resolution (TextureBinding::native_lod). Dolphin's general
// detection (levels compared with box-filtered copies) flags most of GP 2's
// CMPR textures, whose levels are compressed separately. `texels`: the
// decoded levels in RGBA8, one after another.
bool fades_with_distance(const u32* texels, u32 width, u32 height, u32 levels);

}  // namespace gx
