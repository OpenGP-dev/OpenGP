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
//   * Widescreen widens the EFB on both sides (see set_widescreen()).
//
// Based on Dolphin's VideoCommon (BPStructs.cpp, BPFunctions.cpp,
// RenderState.cpp, PixelShaderManager.cpp; Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
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

  // Guest RAM (MEM1), for reading vertex arrays directly.
  void set_ram(const u8* ram, u32 size) { ram_ = ram, ram_size_ = size; }

  // Where finished frames go. Without an output, only register state is
  // tracked (no vertex or texture work is done).
  void set_output(FrameOutput* output) { output_ = output; }
  // Waits while the output is too far behind (see FrameOutput::wait_for_room).
  void wait_for_output() {
    if (output_) output_->wait_for_room();
  }

  // Widescreen: widens the EFB by `side` pixels on the left and right, from
  // the next frame on (0: the game's 4:3). Thread-safe.
  //   * 3D scenes show more on the sides: the game's pixels keep their size
  //     and place, and the host no longer clips at the game's screen edges.
  //   * 2D (orthographic) draws stay in the middle, except ones that span
  //     the whole screen (backgrounds, fades), which are stretched to the
  //     full width.
  //   * Scissors, clears and copies that span the whole width do so in the
  //     wide EFB, so full-screen copies (and the displayed frame) are wide.
  void set_widescreen(u32 side) { requested_side_.store(side, std::memory_order_relaxed); }

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
  // Profiling counters (times in ns). The draw time includes the state,
  // texture and transform times.
  struct Stats {
    std::atomic<u64> draw_ns{0}, state_ns{0}, texture_ns{0}, transform_ns{0}, vertices{0};
    std::atomic<u64> readback_ns{0}, readbacks{0};  // EFB reads that waited for the renderer
  };
  const Stats& stats() const { return stats_; }

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
  u32 efb_width() const { return kEfbWidth + 2 * side_; }
  bool widen_2d(u32 first_vertex);
  void extend_edges(u32 first_vertex);
  void widen_overlay(u32 first_vertex, const DrawState& state);
  PixelFormat pixel_format() const { return static_cast<PixelFormat>(bits(bp_[BP_ZCOMPARE], 0, 3)); }

  TextureBinding bind_texture(u32 map, s32* size);
  u32 new_handle() { return next_handle_++; }

  void assemble(u8 primitive, u32 base, u16 count, const Viewport& vp);

  MemoryReader read_;
  void* context_;
  const u8* ram_ = nullptr;
  u32 ram_size_ = 0;
  FrameOutput* output_ = nullptr;
  std::unique_ptr<Frame> frame_;
  u64 frames_ = 0;
  // OPENGP_GX_DUMP=<frame>: log that frame's draws, copies and clears.
  Stats stats_;
  // Widescreen: pixels added on each side of the EFB (fixed within a frame).
  std::atomic<u32> requested_side_{0};
  u32 side_ = 0;
  bool new_display_frame_ = true;
  // The latest run of 2D draws that may make up a full-width background, in
  // game pixels (see widen_2d()), and where its vertices start.
  struct Run {
    bool active = false;
    u32 first_vertex = 0;
    float left = 0, right = 0, top = 0, bottom = 0;
  } run_;
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
  // GX clip-space positions of the current draw (for culling).
  std::vector<std::array<float, 4>, UninitAllocator<std::array<float, 4>>> clip_;
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
    bool native_lod;  // see TextureBinding::native_lod
  };
  std::unordered_map<u64, CachedTexture> textures_;
  // EFB copies, by destination address.
  struct CopiedTexture {
    u32 handle;
    u32 width, height;
    TexFormat format;
    u64 memory_hash;  // of the destination memory when copied
    u64 last_used;    // frame it was last copied or sampled
    bool xfb;
  };
  std::unordered_map<u32, CopiedTexture> copies_;
  // Hashes of guest memory ranges (address, size) taken this frame. Hashing
  // is the expensive part of binding a texture, and games bind the same ones
  // many times a frame; memory is assumed not to change within a frame.
  struct MemoryHash {
    u64 frame;  // frames_ + 1 when taken
    u64 hash;
  };
  std::unordered_map<u64, MemoryHash> memory_hashes_;
  u64 memory_hash(const u8* data, u32 address, u32 size);
  u32 next_handle_ = 1;
  std::vector<u32> scratch_;

  // EFB contents read back for peeks (colour, depth). With OPENGP_EFB_SYNC,
  // peeks wait for a readback of the EFB as it is, cached until the EFB
  // changes; otherwise they read the newest readback that has completed,
  // requested once a frame (usually one frame old).
  struct EfbPeeks {
    std::shared_ptr<EfbReadback> current;
    std::deque<std::shared_ptr<EfbReadback>> pending;
    bool requested = false;  // this frame
  };
  EfbPeeks peeks_[2];
  bool efb_sync_ = false;
  void efb_changed() {
    if (efb_sync_) peeks_[0].current.reset(), peeks_[1].current.reset();
  }
};

}  // namespace gx
