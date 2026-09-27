// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GX rendering front end: keeps the BP, XF and TMEM state the command
// stream sets up, and turns draws, EFB copies and clears into Frames for
// the renderer (see gx/frame.h).
//
//   * Vertices are loaded and transformed on the CPU (vertex_loader.h,
//     transform.h); the renderer gets clip-space positions, lit colours and
//     generated texture coordinates.
//   * The pixel pipeline state of each draw becomes the uniforms of one TEV
//     ubershader, plus a small pipeline key (blending, depth).
//   * Textures are decoded from guest memory to RGBA8 and cached by content.
//   * EFB copies stay on the GPU: a copy produces a renderer texture,
//     registered at its destination address. A later texture read at that
//     address uses it, as long as the memory there has not been rewritten
//     (the copy itself never writes guest memory).
//
// Based on Dolphin's VideoCommon (BPStructs.cpp, BPFunctions.cpp,
// RenderState.cpp, PixelShaderManager.cpp; Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "gx/frame.h"
#include "gx/transform.h"
#include "gx/vertex_loader.h"

namespace gx {

class Frontend {
 public:
  using MemoryReader = VertexLoader::MemoryReader;

  Frontend(MemoryReader read, void* context);
  ~Frontend();

  // Where finished frames go. Without an output, only register state is
  // tracked (no vertex or texture work is done).
  void set_output(FrameOutput* output) { output_ = output; }

  // Command stream input (see gx::Decoder). `cp` is the CP register file.
  void bp_write(u8 reg, u32 value);
  void xf_write(u16 address, std::span<const u8> words);
  void indexed_load(u8 opcode, u32 value, const u32* cp);
  void draw(u8 primitive, u8 vat, u16 count, std::span<const u8> vertices, const u32* cp);

  // CPU access to the EFB (0xC8000000-0xC8FFFFFF). Reads render everything
  // so far and read the EFB back (cached until the next draw).
  // `alpha_mode`: PE alpha read mode (0: alpha 00, 1: FF, 2: as stored).
  u32 efb_read(u32 address, u32 alpha_mode);
  void efb_write(u32 address, u32 value);

  u32 bp(u8 reg) const { return bp_[reg]; }
  const XfUnit& xf() const { return xf_; }
  u64 frames() const { return frames_; }

 private:
  struct Viewport;  // viewport and scissor offset mapping for the current draw

  Frame& frame();
  void end_frame(bool present);

  void efb_copy(u32 trigger);
  void clear(u32 x, u32 y, u32 width, u32 height);
  void load_tlut();
  void preload_tmem();

  u32 draw_state();
  void build_uniforms(PixelUniforms& u) const;
  PipelineKey pipeline_key() const;
  void scissor(s32* rect) const;
  PixelFormat pixel_format() const { return static_cast<PixelFormat>(bits(bp_[BP_ZCOMPARE], 0, 3)); }

  TextureBinding bind_texture(u32 map, s32* size);
  u32 new_handle() { return next_handle_++; }

  void assemble(u8 primitive, u32 base, u16 count, const Viewport& vp);

  MemoryReader read_;
  void* context_;
  FrameOutput* output_ = nullptr;
  std::unique_ptr<Frame> frame_;
  u64 frames_ = 0;
  // OPENGP_GX_DUMP=<frame>: log that frame's draws, copies and clears.
  u64 dump_frame_ = ~0ull;
  bool dumping_ = false;
  u32 dumped_state_ = ~0u;

  // Registers
  std::array<u32, 256> bp_{};
  u32 bp_mask_ = 0xFFFFFF;
  XfUnit xf_;
  std::vector<u8> tmem_;
  s32 tev_colors_[4][4] = {};  // TEV registers PREV, C0-C2 (loaded through BP 0xE0-0xE7)
  s32 tev_konst_[4][4] = {};   // konst colours K0-K3 (same registers, "constant" type)

  // Per-draw work
  VertexLoader loader_;
  InputVertex input_{};  // kept across vertices: absent normals reuse the last values
  std::vector<std::array<float, 4>> clip_;  // GX clip-space positions of the current draw (for culling)
  std::vector<u8> skip_;

  // State caching: rebuilt when registers change.
  bool state_dirty_ = true;
  bool textures_dirty_ = true;
  DrawState state_;
  s32 texsizes_[8][2] = {};
  u32 state_index_ = ~0u;  // index of state_ in the current frame's states
  PixelUniforms uniforms_{};

  // Texture cache: decoded textures by content (address, format, size, data and palette hashes).
  struct CachedTexture {
    u32 handle;
    u64 last_used;
  };
  std::unordered_map<u64, CachedTexture> textures_;
  // EFB copies, by destination address.
  struct CopiedTexture {
    u32 handle;
    u32 width, height;
    TexFormat format;
    u64 memory_hash;  // of the destination memory when copied
  };
  std::unordered_map<u32, CopiedTexture> copies_;
  u32 next_handle_ = 1;
  std::vector<u32> scratch_;

  // EFB contents read back for peeks; dropped when the EFB changes.
  std::shared_ptr<EfbReadback> efb_color_, efb_depth_;
  void efb_changed() { efb_color_.reset(), efb_depth_.reset(); }
};

}  // namespace gx
