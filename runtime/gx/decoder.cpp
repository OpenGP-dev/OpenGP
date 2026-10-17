// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/decoder.h"

#include <bit>
#include <cstdio>

namespace gx {

namespace {

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
u32 be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }

// Vertex attribute encodings in the vertex descriptor.
enum : u32 { NOT_PRESENT = 0, DIRECT = 1, INDEX8 = 2, INDEX16 = 3 };

// Bytes per component for the component formats (u8, s8, u16, s16, f32; 5-7 behave as f32).
constexpr u32 kComponentSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};
// Bytes per color for the color formats (RGB565, RGB888, RGB888x, RGBA4444, RGBA6666, RGBA8888).
constexpr u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }

u32 index_size(u32 type) { return type == INDEX8 ? 1 : type == INDEX16 ? 2 : 0; }

}  // namespace

u32 Decoder::vertex_size(u8 vat) const {
  u32& cached = vertex_sizes_[vat & 7];
  if (cached) return cached;
  const u32 lo = cp_[CP_VCD_LO];
  const u32 hi = cp_[CP_VCD_HI];
  const u32 a = cp_[CP_VAT_A + (vat & 7)];
  const u32 b = cp_[CP_VAT_B + (vat & 7)];
  const u32 c = cp_[CP_VAT_C + (vat & 7)];

  // Matrix indices: one byte each for the position and texture matrices.
  u32 size = static_cast<u32>(std::popcount(lo & 0x1FF));

  // Position: 2 or 3 components.
  if (const u32 type = bits(lo, 9, 2); type == DIRECT) size += (bits(a, 0, 1) ? 3 : 2) * kComponentSize[bits(a, 1, 3)];
  else size += index_size(type);

  // Normal: 3 (N) or 9 (NBT) components; indexed NBT may use three indices.
  if (const u32 type = bits(lo, 11, 2); type == DIRECT) {
    size += (bits(a, 9, 1) ? 9 : 3) * kComponentSize[bits(a, 10, 3)];
  } else if (type != NOT_PRESENT) {
    size += index_size(type) * ((bits(a, 9, 1) && bits(a, 31, 1)) ? 3 : 1);
  }

  // Colors.
  for (u32 i = 0; i < 2; ++i) {
    const u32 type = bits(lo, 13 + 2 * i, 2);
    if (type == DIRECT) size += kColorSize[bits(a, 14 + 4 * i, 3)];
    else size += index_size(type);
  }

  // Texture coordinates: 1 or 2 components. Element and format fields per coordinate.
  struct Field {
    u32 word;
    u32 shift;  // elements bit; format is the next 3 bits
  };
  const Field tex[8] = {{a, 21}, {b, 0}, {b, 9}, {b, 18}, {b, 27}, {c, 5}, {c, 14}, {c, 23}};
  for (u32 i = 0; i < 8; ++i) {
    const u32 type = bits(hi, 2 * i, 2);
    if (type == DIRECT) size += (bits(tex[i].word, tex[i].shift, 1) ? 2 : 1) * kComponentSize[bits(tex[i].word, tex[i].shift + 1, 3)];
    else size += index_size(type);
  }
  cached = size;  // never 0: a vertex has a position
  return size;
}

std::size_t Decoder::run(std::span<const u8> data, bool in_display_list) {
  std::size_t pos = 0;
  while (pos < data.size()) {
    const u8* p = data.data() + pos;
    const std::size_t left = data.size() - pos;
    const u8 op = p[0];
    if (!in_display_list) command_offset_ = pos;
    std::size_t length = 1;

    if (op >= GX_PRIMITIVE_START && op < 0xC0) {
      if (left < 3) break;
      const u16 count = be16(p + 1);
      const u8 vat = op & 7;
      const std::size_t bytes = std::size_t{count} * vertex_size(vat);
      length = 3 + bytes;
      if (left < length) break;
      sink_.draw(static_cast<u8>((op >> 3) & 7), vat, count, std::span{p + 3, bytes});
    } else {
      switch (op) {
        case GX_NOP:
        case GX_UNKNOWN_RESET:
        case GX_UNKNOWN_METRICS:
        case GX_INVALIDATE_VTX_CACHE: break;
        case GX_LOAD_CP_REG:
          length = 6;
          if (left < length) return pos;
          cp_[p[1]] = be32(p + 2);
          vertex_sizes_.fill(0);
          sink_.cp_write(p[1], be32(p + 2));
          break;
        case GX_LOAD_XF_REG: {
          if (left < 5) return pos;
          const u32 header = be32(p + 1);
          const u32 count = ((header >> 16) & 0xF) + 1;
          length = 5 + 4 * std::size_t{count};
          if (left < length) return pos;
          sink_.xf_write(static_cast<u16>(header), std::span{p + 5, 4 * std::size_t{count}});
          break;
        }
        case GX_LOAD_INDX_A:
        case GX_LOAD_INDX_B:
        case GX_LOAD_INDX_C:
        case GX_LOAD_INDX_D:
          length = 5;
          if (left < length) return pos;
          sink_.indexed_load(op, be32(p + 1));
          break;
        case GX_CALL_DL: {
          length = 9;
          if (left < length) return pos;
          const u32 address = be32(p + 1);
          const u32 size = be32(p + 5);
          if (in_display_list) {
            std::fprintf(stderr, "[gx] nested display list call ignored\n");
          } else if (const auto dl = read_dl_(context_, address, size); !dl.empty()) {
            const std::size_t used = run(dl, true);
            if (used != dl.size()) std::fprintf(stderr, "[gx] display list %08X: %zu of %u bytes decoded\n", address, used, size);
          }
          break;
        }
        case GX_LOAD_BP_REG: {
          length = 5;
          if (left < length) return pos;
          const u32 v = be32(p + 1);
          sink_.bp_write(static_cast<u8>(v >> 24), v & 0xFFFFFF);
          break;
        }
        default:
          // Treat as a one-byte command (as Dolphin does) and report it.
          if (unknown_++ < 16) std::fprintf(stderr, "[gx] unknown opcode %02X\n", op);
          break;
      }
    }
    pos += length;
    ++commands_;
  }
  return pos;
}

}  // namespace gx
