// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoder for the GX command stream (what the CPU writes to the graphics
// FIFO and what display lists contain): register loads for the command
// processor (CP), transform unit (XF) and blitting processor (BP), indexed
// loads, display list calls and primitives.
//
// It keeps the CP vertex state needed to know how long each primitive is,
// and reports register writes and draws to a Sink (the renderer, later).
// Based on Dolphin's OpcodeDecoding.cpp and VertexLoaderBase.cpp (Copyright
// Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "gekko/types.h"

namespace gx {

using namespace gekko;

// Command opcodes.
enum : u8 {
  GX_NOP = 0x00,
  GX_UNKNOWN_RESET = 0x01,
  GX_LOAD_CP_REG = 0x08,
  GX_LOAD_XF_REG = 0x10,
  GX_LOAD_INDX_A = 0x20,
  GX_LOAD_INDX_B = 0x28,
  GX_LOAD_INDX_C = 0x30,
  GX_LOAD_INDX_D = 0x38,
  GX_CALL_DL = 0x40,
  GX_UNKNOWN_METRICS = 0x44,
  GX_INVALIDATE_VTX_CACHE = 0x48,
  GX_LOAD_BP_REG = 0x61,
  GX_PRIMITIVE_START = 0x80,  // 0x80-0xBF: primitive type in bits 3-5, VAT in bits 0-2
};

// CP registers used for vertex decoding.
enum : u8 {
  CP_VCD_LO = 0x50,
  CP_VCD_HI = 0x60,
  CP_VAT_A = 0x70,  // + VAT index
  CP_VAT_B = 0x80,
  CP_VAT_C = 0x90,
  CP_ARRAY_BASE = 0xA0,  // + array
  CP_ARRAY_STRIDE = 0xB0,
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void bp_write(u8 reg, u32 value) { (void)reg, (void)value; }
  virtual void cp_write(u8 reg, u32 value) { (void)reg, (void)value; }
  virtual void xf_write(u16 address, std::span<const u8> words) { (void)address, (void)words; }
  virtual void indexed_load(u8 opcode, u32 value) { (void)opcode, (void)value; }
  // `vertices`: count * vertex_size bytes in the current vertex format.
  virtual void draw(u8 primitive, u8 vat, u16 count, std::span<const u8> vertices) {
    (void)primitive, (void)vat, (void)count, (void)vertices;
  }
};

class Decoder {
 public:
  // `read_display_list(address, size)` returns the bytes of a display list
  // in guest memory (or an empty span if the range is invalid).
  using DisplayListReader = std::span<const u8> (*)(void* context, u32 address, u32 size);

  Decoder(Sink& sink, DisplayListReader read_dl, void* context) : sink_(sink), read_dl_(read_dl), context_(context) {}

  // Decodes the complete commands at the start of `data`; returns how many
  // bytes they used. The rest is an incomplete command (call again with more).
  std::size_t run(std::span<const u8> data) { return run(data, false); }

  u32 cp(u8 reg) const { return cp_[reg]; }
  const u32* cp_regs() const { return cp_.data(); }
  u32 vertex_size(u8 vat) const;
  u64 commands() const { return commands_; }
  u64 unknown_opcodes() const { return unknown_; }
  // Where the command being decoded starts in the data given to run() (for
  // a display list's contents: where the call to it starts).
  std::size_t command_offset() const { return command_offset_; }

 private:
  std::size_t run(std::span<const u8> data, bool in_display_list);

  Sink& sink_;
  DisplayListReader read_dl_;
  void* context_;
  std::array<u32, 256> cp_{};
  // vertex_size() per attribute table (0: not worked out since the last CP
  // register write). Every primitive needs it.
  mutable std::array<u32, 8> vertex_sizes_{};
  u64 commands_ = 0;
  u64 unknown_ = 0;
  std::size_t command_offset_ = 0;
};

}  // namespace gx
