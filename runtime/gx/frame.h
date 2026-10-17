// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the GX front end hands to the renderer: one Frame per displayed
// frame (it ends with a copy to the external framebuffer), or a partial one
// when the CPU needs to read the EFB. A frame holds
// fully transformed vertices, the pixel pipeline state of each draw as
// uniforms for the TEV ubershader, texture uploads and an ordered list of
// commands: draws, EFB clears and EFB copies.
//
// Everything here is plain data, so frames can be built on the guest thread
// and rendered on another thread without sharing any GX state.
//
// Widescreen: the EFB can be wider than the game's 640 pixels, with the
// game's EFB in the middle (Frame::efb_width). Rectangles in a frame (scissors,
// clears, copies) are in the wide EFB's pixels; ones that span the game's
// whole width span the wide EFB.
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gx/regs.h"

namespace gx {

// A transformed vertex, in the layout the renderer's vertex shader reads.
struct Vertex {
  float pos[4];     // clip space: x and y in host NDC (+y up) times w; z = EFB depth (0..1) times w
  u32 color[2];     // lit colours, RGBA8 (red in the lowest byte)
  float tex[8][3];  // texture coordinates s, t, q (not yet scaled to texels)
};
static_assert(sizeof(Vertex) == 120);

// Uniforms of the TEV ubershader (std140: every member is 16-byte aligned).
// Field meanings follow Dolphin's pixel shader constants.
struct PixelUniforms {
  s32 colors[4][4];     // TEV registers PREV, C0, C1, C2 (s11 values)
  s32 konst[32][4];     // konst colour table, indexed by the konst selection
  s32 alpha_ref[4];     // x: alpha test reference 0, y: reference 1
  s32 texsize[8][4];    // per texture map: x width, y height
  s32 tcscale[8][4];    // per texture coordinate: x/y texel scale (SU_SSIZE/SU_TSIZE + 1)
  s32 indscale[2][4];   // indirect texture coordinate scale shifts
  s32 indmtx[6][4];     // indirect matrices: rows xyz, w shift
  s32 fog_color[4];
  s32 fog_i[4];         // y: B magnitude, w: B shift
  float fog_f[4];       // x: A, y: C, z: range centre, w: viewport width
  float fog_range[3][4];  // range adjustment table (10 values)
  u32 stages[16][4];    // x: colour combiner, y: alpha combiner, z: indirect command, w: iref (per indirect stage)
  u32 orders[8][4];     // x: TEV order (two stages), y: konst/swap select
  u32 misc[4];          // x: genmode, y: alpha test, z: fog param 3, w: fog range base
  u32 misc2[4];         // x: dst alpha, y: rgba6 EFB, z: dither, w: logic op (bit 4 enable, bits 0-3 mode)
};
static_assert(sizeof(PixelUniforms) % 16 == 0);

// Host pipeline state for a draw, packed into one integer (see the
// accessors). Everything not listed is handled in the shaders.
struct PipelineKey {
  u32 bits = 0;
  // blend: bit 0 enable, 1-3 src, 4-6 dst, 7-9 src alpha, 10-12 dst alpha,
  //        13 subtract, 14 subtract alpha, 15 colour write, 16 alpha write
  // depth: bit 17 test, 18-20 function, 21 write
  // bit 22: early depth test (depth is tested and written before the alpha test)
  //
  // Blend factors use the GX encoding: 0 zero, 1 one, 2 other colour, 3
  // inverse other colour, 4 src alpha, 5 inverse src alpha, 6 dst alpha, 7
  // inverse dst alpha ("other" is dst for the source factor, src for the dest).
  bool operator==(const PipelineKey&) const = default;
};

struct TextureBinding {
  u32 texture = 0;  // texture handle; 0 = nothing bound
  u32 mode0 = 0;    // TX_SETMODE0: wrap, filter, LOD bias
  u32 mode1 = 0;    // TX_SETMODE1: LOD range
  // The mip levels aren't downscaled copies of each other but an effect (GP
  // 2's rainbows fade in with distance this way): choose them as at the
  // game's resolution, not the internal one.
  bool native_lod = false;
  bool operator==(const TextureBinding&) const = default;
};

struct DrawState {
  PipelineKey pipeline;
  u32 uniforms = 0;  // index into Frame::uniforms
  TextureBinding textures[8];
  s32 scissor[4] = {};  // x, y, width, height in (wide) EFB pixels
  bool operator==(const DrawState&) const = default;
};

struct DrawCmd {
  u32 state;  // index into Frame::states
  u32 first_index, index_count;
};

// Clears a rectangle of the EFB (after a copy with the clear bit set).
struct ClearCmd {
  u32 x, y, width, height;
  bool color, alpha, depth;
  u32 rgba;     // red in the lowest byte
  float z;      // 0..1
};

// Copies a rectangle of the EFB to a texture (or the external framebuffer),
// converting it to what the game would read back from memory.
struct CopyCmd {
  u32 dest;                     // texture handle (the renderer creates it on first use)
  u32 src_x, src_y, src_width, src_height;  // (wide) EFB rectangle
  u32 width, height;            // destination size in EFB pixels (half scale / y scale applied)
  CopyFormat format;
  bool depth;                   // copy the depth buffer (EFB format Z24)
  bool intensity;               // convert RGB to YUV (intensity formats)
  bool efb_has_alpha;           // EFB format RGBA6 (otherwise alpha reads as 255)
  bool clamp_top, clamp_bottom;
  float gamma;                  // XFB copies: 1.0, 1.7 or 2.2
  u32 filter[3];                // vertical filter weights (rows above, current, below; sum 64 = unity)
};

// A texture decoded from guest memory: RGBA8, all mip levels back to back.
struct TextureUpload {
  u32 handle;
  u32 width, height, levels;
  std::size_t offset;  // into Frame::texels
};

struct Command {
  enum class Type : u8 { Draw, Clear, Copy, Readback } type;
  u32 index;  // into draws, clears, copies or readbacks
};

// A request to read the whole EFB back at a point in a frame (for CPU reads
// of the EFB, "peeks"). The renderer fills it once the GPU has got there;
// `urgent` requests are completed before the renderer moves on (the guest
// thread is waiting for them).
class EfbReadback {
 public:
  EfbReadback(bool depth, bool urgent) : depth(depth), urgent(urgent) {}
  const bool depth, urgent;
  // kEfbWidth * kEfbHeight values (the game's EFB, without the widescreen
  // sides): RGBA8 colours (red in the lowest byte) or 24-bit depths. Filled
  // by the renderer before complete().
  std::vector<u32> pixels;

  void complete() {
    std::lock_guard lock(mutex_);
    done_ = true;
    cv_.notify_all();
  }
  bool ready() {
    std::lock_guard lock(mutex_);
    return done_;
  }
  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return done_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
};

// What the front end did for a displayed frame, for profiling: everything
// since the previous displayed frame, so it includes the partial frames
// before it (which have none of their own). Times in nanoseconds.
struct FrameStats {
  u64 finished_ns = 0;  // steady clock when the frame was finished (its XFB copy)
  u64 interval_ns = 0;  // since the previous displayed frame was finished
  // Front end work on the game's thread: draws (including the state,
  // texture and transform work below), texture decoding (part of the
  // texture work), and waits for the renderer and for EFB readbacks.
  u64 draw_ns = 0, state_ns = 0, texture_ns = 0, decode_ns = 0, transform_ns = 0;
  u64 wait_ns = 0, readback_ns = 0;
  u32 calls = 0;      // primitives the game drew
  u32 culled = 0;     // ... of which nothing was left after culling (or clipped away by the scissor)
  u32 vertices = 0, triangles = 0;
  u32 binds = 0;      // textures looked up for draws
  u32 copy_binds = 0;  // ... that used an EFB copy
  u32 decodes = 0;    // ... that had to be decoded (not in the cache)
  u64 decoded_bytes = 0;  // RGBA8 bytes decoded
  u64 hashed_bytes = 0;   // guest memory hashed (to find textures in the cache)
  u32 copies = 0, xfb_copies = 0, clears = 0, readbacks = 0;
  u32 partials = 0;   // partial frames (rendered early for EFB readbacks)
  u32 depth_equal = 0;  // draws with an "equal" depth test (sensitive to depth precision)
  // Widescreen: draws the heuristics changed (see Frontend::set_widescreen).
  u32 widened_2d = 0, extended_edges = 0, widened_overlays = 0;
  u32 cached_textures = 0, cached_copies = 0;  // at the end of the frame
  u32 risks = 0;  // see Risk (more than Frame::risks holds)
};

// Something in a frame OpenGP does not draw as the hardware would: a likely
// graphical glitch (for profiling). `address` and `a`, `b` say more.
struct Risk {
  enum class Kind : u8 {
    TextureOutOfRange,    // a texture outside guest memory (drawn white): address, a = bytes
    TextureFormat,        // an invalid texture format (drawn white): address, a = format
    TmemRgba8,            // a preloaded RGBA8 texture (not supported; drawn white): address
    TmemOutOfRange,       // a preloaded texture past the end of TMEM: a = TMEM address, b = bytes
    CopySize,             // a texture where an EFB copy went, of another size: the copy is not used and
                          // memory is read instead, which copies never write (a = texture w<<16|h, b = copy's)
    CopyFormat,           // a texture read from an EFB copy in another format (a = texture's, b = copy's)
    EfbPeekEmpty,         // a CPU read of the EFB before any readback was ready (reads 0): address
    EfbWrite,             // a CPU write to the EFB (not supported): address, a = value
    ZFreeze,              // z-freeze (not supported): a = genmode
    DisplayListRange,     // a display list outside guest memory: address, a = bytes
  };
  Kind kind;
  u32 address = 0, a = 0, b = 0;
};

// A frame captured for inspection (F9, see app/capture.h): what the front end
// knew about each draw, EFB copy and texture. Only captured frames have one.
// In a captured frame each of the game's primitives is its own DrawCmd, and
// every texture is decoded afresh (the texture cache is emptied first), so
// all of them are in Frame::uploads.
struct FrameCapture {
  // Where the game was when it issued a command: return addresses on the
  // guest's stack (the first is its link register), and on Windows the
  // host's stack (recompiled functions are named after the game's).
  struct Stacks {
    u32 guest[8] = {};
    u8 guest_depth = 0;
    u64 host[24] = {};
    u8 host_depth = 0;
  };
  struct Texture {
    u32 address = 0, format = 0, width = 0, height = 0, levels = 1;
    u32 tlut_address = 0, tlut_format = 0;  // TMEM palette (paletted formats)
    bool tmem = false;  // preloaded into TMEM
    bool copy = false;  // an EFB copy (a renderer texture, not decoded from memory)
    u32 copy_format = 0;  // ... in this TexFormat
  };
  struct Draw {
    s32 draw = -1;     // its DrawCmd in Frame::draws, or -1 if nothing was left to draw
    u32 state = 0;     // Frame::states
    u32 primitive = 0, vat = 0, count = 0;
    u32 first_vertex = 0, vertex_count = 0;  // Frame::vertices (widescreen additions included)
    u32 triangles = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds in game pixels (x beyond 0..640 in widescreen)
    float z0 = 0, z1 = 0;                  // EFB depth range, 0..1
    float w0 = 0, w1 = 0;
    u32 widescreen = 0;  // 1 widened 2D, 2 extended edges, 3 widened overlay
    bool perspective = true;
    // Registers the uniforms and pipeline key don't show as they were.
    u32 genmode = 0, zmode = 0, blendmode = 0, zcompare = 0, constant_alpha = 0;
    u32 vcd_lo = 0, vcd_hi = 0, vat_a = 0, vat_b = 0, vat_c = 0;
    u32 xf_colors = 0, xf_texgens = 0, color_ctrl[2] = {}, alpha_ctrl[2] = {}, texgen[8] = {};
    u32 matrix_index[2] = {};
    float viewport[6] = {}, projection[7] = {};
    Stacks stacks;
  };
  struct Copy {
    u32 command = 0;  // Frame::commands
    u32 address = 0, x = 0, y = 0, width = 0, height = 0;  // the game's rectangle and destination
    u32 trigger = 0, pixel_format = 0;
    Stacks stacks;
  };
  std::vector<Draw> draws;
  std::vector<Copy> copies;
  std::unordered_map<u32, Texture> textures;  // by handle
  u32 side = 0;      // widescreen pixels added on each side
  u32 partials = 0;  // partial frames before the end (their draws are not in the capture)
};

// An allocator that leaves trivially constructible elements uninitialised
// when a vector grows (vertices are written right after).
template <typename T>
struct UninitAllocator : std::allocator<T> {
  template <typename U>
  struct rebind {
    using other = UninitAllocator<U>;
  };
  UninitAllocator() = default;
  template <typename U>
  UninitAllocator(const UninitAllocator<U>&) {}
  template <typename U>
  void construct(U* p) noexcept {
    ::new (static_cast<void*>(p)) U;
  }
  template <typename U, typename... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

struct Frame {
  std::vector<Vertex, UninitAllocator<Vertex>> vertices;
  std::vector<u32, UninitAllocator<u32>> indices;
  std::vector<PixelUniforms> uniforms;
  std::vector<DrawState> states;
  std::vector<DrawCmd> draws;
  std::vector<ClearCmd> clears;
  std::vector<CopyCmd> copies;
  std::vector<Command> commands;
  std::vector<TextureUpload> uploads;  // created before the commands run
  std::vector<u8> texels;
  std::vector<u32> releases;  // texture handles no longer used (destroy after this frame)
  u32 xfb = 0;                // XFB copy to display (texture handle), 0 if none
  u32 xfb_width = 0, xfb_height = 0;
  float xfb_aspect = 4.0f / 3.0f;  // the XFB's display aspect ratio
  u32 efb_width = kEfbWidth;       // 640, or more for widescreen (see above)
  u64 number = 0;             // displayed frames before this one (partial frames share the number)
  u64 finished_ns = 0;        // steady clock when it was finished (displayed frames only)
  std::vector<std::shared_ptr<EfbReadback>> readbacks;
  FrameStats stats;          // displayed frames only
  std::vector<Risk> risks;   // the first few of the frame's (displayed frames only)
  std::unique_ptr<FrameCapture> capture;  // captured frames only (see FrameCapture)

  // Empties the frame for reuse, keeping the buffers' capacity.
  void clear() {
    vertices.clear();
    indices.clear();
    uniforms.clear();
    states.clear();
    draws.clear();
    clears.clear();
    copies.clear();
    commands.clear();
    uploads.clear();
    texels.clear();
    releases.clear();
    xfb = xfb_width = xfb_height = 0;
    xfb_aspect = 4.0f / 3.0f;
    efb_width = kEfbWidth;
    number = finished_ns = 0;
    readbacks.clear();
    stats = {};
    risks.clear();
    capture.reset();
  }
};

// Receives finished frames (called on the thread that runs guest code).
class FrameOutput {
 public:
  virtual ~FrameOutput() = default;
  // Takes a frame; never blocks.
  virtual void submit(std::unique_ptr<Frame> frame) = 0;
  // Blocks while the renderer is too far behind, which paces the game. Called
  // on the game's thread with no lock held. Returns how long it waited (ns).
  virtual u64 wait_for_room() { return 0; }
  // A rendered frame to reuse (its buffers keep their capacity), or nullptr.
  virtual std::unique_ptr<Frame> recycle() { return nullptr; }
};

}  // namespace gx
