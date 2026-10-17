// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GX front end's vertex pipeline (vertex loader, XF transform, the
// viewport, primitive assembly and the widescreen adjustments), fed random
// vertex formats, attribute layouts, matrices, lights, texture coordinate
// generators and primitives. The vertices and indices of the frames it makes
// must hash to known values: optimisations of this path must not change a
// single bit of what reaches the renderer.
//
// The values hold for x86-64, where every compiler does the same IEEE single
// precision operations in the order written. If a change to the maths is
// intended, update them (and say why in the commit).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "gx/frontend.h"
#include "gx/regs.h"

using namespace gx;

namespace {

struct Rng {
  u64 s;
  u64 next() {
    u64 z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  u32 below(u32 n) { return static_cast<u32>(next() % n); }
  bool chance(u32 percent) { return below(100) < percent; }
  // A finite float in [lo, hi), from 24 random bits.
  float real(float lo, float hi) {
    return lo + (hi - lo) * static_cast<float>(next() >> 40) * (1.0f / 16777216.0f);
  }
};

constexpr u32 kRamSize = 32 << 20;
constexpr u32 kArrayBase = 0x00100000;  // 16 CP arrays of 1 MiB from here
constexpr u32 kArraySize = 1 << 20;

struct Output : FrameOutput {
  std::vector<std::unique_ptr<Frame>> frames, spare;
  void submit(std::unique_ptr<Frame> frame) override { frames.push_back(std::move(frame)); }
  std::unique_ptr<Frame> recycle() override {
    if (spare.empty()) return nullptr;
    auto f = std::move(spare.back());
    spare.pop_back();
    return f;
  }
};

struct Hash {
  u64 h = 0xCBF29CE484222325ull;
  void add(const void* data, std::size_t size) {
    const u8* p = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  }
};

constexpr u32 kComponentSize[8] = {1, 1, 2, 2, 4, 4, 4, 4};
constexpr u32 kColorSize[8] = {2, 3, 4, 2, 3, 4, 4, 4};

class Test {
 public:
  explicit Test(u64 seed) : rng_{seed}, ram_(kRamSize) {
    // Arrays: every aligned word a modest float, so float attributes are
    // finite whatever the index; integer formats read their bytes.
    for (u32 a = kArrayBase; a < kArrayBase + 16 * kArraySize; a += 4) put32(a, std::bit_cast<u32>(rng_.real(-64, 64)));
    for (u32 i = 0; i < 16; ++i) {
      cp_[0xA0 + i] = kArrayBase + i * kArraySize;
      cp_[0xB0 + i] = 4 * (1 + rng_.below(16));  // stride: whole words, up to 64 bytes
    }
    frontend_.set_ram(ram_.data(), kRamSize);
    frontend_.set_output(&output_);
    bp(BP_SCISSOR_TL, 0);
    bp(BP_SCISSOR_BR, (639u << 12) | 527u);
    bp(BP_SCISSOR_OFFSET, 0);
  }

  // Draws `draws` random primitives as one frame, with the EFB widened by
  // `side` pixels; returns the frame's hash.
  u64 frame(u32 draws, u32 side, bool game_like = false) {
    game_like_ = game_like;
    frontend_.set_widescreen(side);
    for (u32 d = 0; d < draws; ++d) {
      if (d % 8 == 0) random_xf();
      bp(BP_GENMODE, rng_.below(4) << 14);  // cull mode
      random_draw();
    }
    // Copy to the XFB: ends the frame.
    bp(BP_EFB_TL, 0);
    bp(BP_EFB_WH, 639u | (479u << 10));
    bp(BP_COPY_Y_SCALE, 256);
    bp(BP_EFB_ADDR, 0x00F00000 >> 5);
    bp(BP_TRIGGER_EFB_COPY, 1u << 14);
    REQUIRE(!output_.frames.empty());
    const Frame& f = *output_.frames.back();
    Hash h;
    h.add(f.vertices.data(), f.vertices.size() * sizeof(Vertex));
    h.add(f.indices.data(), f.indices.size() * sizeof(f.indices[0]));
    const u64 counts[2] = {f.vertices.size(), f.indices.size()};
    h.add(counts, sizeof(counts));
    vertices_ += f.vertices.size();
    return h.h;
  }

  u64 vertices() const { return vertices_; }

  // The benchmark's workload: what the busiest courses draw most (81% of
  // Arena Stadium's vertices): indexed float positions, an RGBA8 colour and
  // a float texture coordinate, unlit, one texture coordinate generator with
  // dual texture matrices, in strips of 5-7 vertices and some longer ones. Returns the ns per vertex
  // of the fastest of `rounds` frames of `draws` draws.
  double benchmark(u32 draws, u32 rounds, u32 short_percent = 70) {
    bp(BP_EFB_TL, 0);
    bp(BP_EFB_WH, 639u | (479u << 10));
    bp(BP_COPY_Y_SCALE, 256);
    bp(BP_EFB_ADDR, 0x00F00000 >> 5);
    random_xf();
    xf(XF_NUM_COLORS, 1);
    xf(XF_COLOR0_CTRL, 1 << 0);  // material from the vertex, unlit
    xf(XF_ALPHA0_CTRL, 1 << 0);
    xf(XF_COLOR0_CTRL + 1, 0);
    xf(XF_ALPHA0_CTRL + 1, 0);
    xf(XF_DUAL_TEX, 1);  // as GP 2 has it
    xf(XF_NUM_TEXGENS, 1);
    xf(XF_TEXGEN, 5 << 7);  // regular, from texture coordinate 0
    xf(XF_POST_TEXGEN, 0);
    xff(XF_PROJECTION + 0, 1.2f), xff(XF_PROJECTION + 1, 0.0f), xff(XF_PROJECTION + 2, 1.6f);
    xff(XF_PROJECTION + 3, 0.0f), xff(XF_PROJECTION + 4, -1.0001f), xff(XF_PROJECTION + 5, -0.5f);
    xf(XF_PROJECTION + 6, 0);
    cp_[0x50] = (3u << 9) | (3u << 13);  // position and colour 0, index16
    cp_[0x60] = 3;                        // texture coordinate 0, index16
    cp_[0x70] = 0x41377009;  // GP 2's: float positions and texture coordinates, RGBA8 colours
    std::vector<std::vector<u8>> streams;
    std::vector<u16> counts;
    for (u32 d = 0; d < draws; ++d) {
      const u16 count = static_cast<u16>(rng_.chance(short_percent) ? 5 + rng_.below(3) : 20 + rng_.below(40));
      std::vector<u8> st;
      // Meshes index their arrays mostly in order.
      const u32 base = rng_.below(4000);
      for (u32 v = 0; v < count; ++v)
        for (u32 a = 0; a < 3; ++a) {
          const u32 index = base + v;
          st.push_back(static_cast<u8>(index >> 8));
          st.push_back(static_cast<u8>(index));
        }
      streams.push_back(std::move(st));
      counts.push_back(count);
    }
    double best = 1e30;
    for (u32 r = 0; r < rounds; ++r) {
      u64 vertices = 0;
      const auto start = std::chrono::steady_clock::now();
      for (u32 d = 0; d < draws; ++d) {
        frontend_.draw(3, 0, counts[d], streams[d], cp_);  // triangle strip
        vertices += counts[d];
      }
      bp(BP_TRIGGER_EFB_COPY, 1u << 14);
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
      best = std::min(best, ns / static_cast<double>(vertices));
      for (auto& f : output_.frames) output_.spare.push_back(std::move(f));
      output_.frames.clear();
    }
    return best;
  }

 private:
  static const u8* read(void* context, u32 addr, u32 size) {
    auto* t = static_cast<Test*>(context);
    addr &= 0x1FFFFFFF;
    return u64{addr} + size <= kRamSize ? t->ram_.data() + addr : nullptr;
  }

  void put32(u32 addr, u32 v) {
    for (u32 i = 0; i < 4; ++i) ram_[addr + i] = static_cast<u8>(v >> (24 - 8 * i));
  }
  void bp(u8 reg, u32 value) { frontend_.bp_write(reg, value); }
  void xf(u16 addr, u32 value) {
    const u8 w[4] = {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16), static_cast<u8>(value >> 8),
                     static_cast<u8>(value)};
    frontend_.xf_write(addr, w);
  }
  void xff(u16 addr, float v) { xf(addr, std::bit_cast<u32>(v)); }

  void random_xf() {
    // Position/texture matrices (rows 0-63) and post-transform matrices.
    for (u16 i = 0; i < 64 * 4; ++i) xff(XF_POS_MATRICES + i, (i % 4 == 3) ? rng_.real(-50, 50) : rng_.real(-2, 2));
    for (u16 i = 0; i < 32 * 3; ++i) xff(XF_NORMAL_MATRICES + i, rng_.real(-1.5f, 1.5f));
    for (u16 i = 0; i < 64 * 4; ++i) xff(XF_POST_MATRICES + i, rng_.real(-2, 2));
    for (u16 l = 0; l < 8; ++l) {
      const u16 base = XF_LIGHTS + 16 * l;
      xf(base + 3, static_cast<u32>(rng_.next()));
      for (u16 k = 4; k < 16; ++k) xff(base + k, rng_.real(-100, 100));
    }
    xf(XF_NUM_COLORS, rng_.below(3));
    for (u16 c = 0; c < 2; ++c) {
      xf(XF_AMBIENT0 + c, static_cast<u32>(rng_.next()));
      xf(XF_MATERIAL0 + c, static_cast<u32>(rng_.next()));
      // Light masks, lighting enable, sources, diffuse and attenuation functions.
      xf(XF_COLOR0_CTRL + c, rng_.chance(40) ? static_cast<u32>(rng_.next()) & 0x7FFF : rng_.below(2) << 6 | rng_.below(2));
      xf(XF_ALPHA0_CTRL + c, rng_.chance(30) ? static_cast<u32>(rng_.next()) & 0x7FFF : rng_.below(2));
    }
    xf(XF_DUAL_TEX, rng_.below(2));
    xf(XF_MATRIX_INDEX_A, static_cast<u32>(rng_.next()) & 0x3FFFFFFF);
    xf(XF_MATRIX_INDEX_B, static_cast<u32>(rng_.next()) & 0x00FFFFFF);
    // The game's usual viewport.
    xff(XF_VIEWPORT + 0, 320.0f);
    xff(XF_VIEWPORT + 1, -264.0f);
    xff(XF_VIEWPORT + 2, 16777215.0f);
    xff(XF_VIEWPORT + 3, 342.0f + 320.0f);
    xff(XF_VIEWPORT + 4, 342.0f + 264.0f);
    xff(XF_VIEWPORT + 5, 16777215.0f);
    if (rng_.chance(70)) {  // perspective
      xff(XF_PROJECTION + 0, rng_.real(0.8f, 2.0f));
      xff(XF_PROJECTION + 1, rng_.real(-0.2f, 0.2f));
      xff(XF_PROJECTION + 2, rng_.real(0.8f, 2.0f));
      xff(XF_PROJECTION + 3, rng_.real(-0.2f, 0.2f));
      xff(XF_PROJECTION + 4, rng_.real(-1.01f, -1.0f));
      xff(XF_PROJECTION + 5, rng_.real(-2.0f, -0.1f));
      xf(XF_PROJECTION + 6, 0);
    } else {  // orthographic (2D)
      xff(XF_PROJECTION + 0, rng_.real(0.002f, 0.004f));
      xff(XF_PROJECTION + 1, rng_.real(-1.1f, -0.9f));
      xff(XF_PROJECTION + 2, rng_.real(-0.005f, -0.003f));
      xff(XF_PROJECTION + 3, rng_.real(0.9f, 1.1f));
      xff(XF_PROJECTION + 4, rng_.real(-0.001f, 0.0f));
      xff(XF_PROJECTION + 5, rng_.real(-1.0f, 0.0f));
      xf(XF_PROJECTION + 6, 1);
    }
    const u32 texgens = rng_.below(9);
    xf(XF_NUM_TEXGENS, texgens);
    for (u16 i = 0; i < 8; ++i) {
      // Mostly regular texgens from a texture coordinate or the position.
      u32 v;
      if (rng_.chance(80)) {
        const u32 source = rng_.chance(70) ? 5 + rng_.below(8) : rng_.below(5);
        v = (rng_.below(2) << 1) | (rng_.below(2) << 2) | (source << 7);
      } else {
        // Emboss and colour texgens too (types 1-3; 4-7 are not valid). An
        // emboss generator's source must be an earlier generator.
        const u32 type = 1 + rng_.below(3);
        v = (static_cast<u32>(rng_.next()) & 0x38F8E) | (type << 4);
        if (type == 1) v = i ? v | (rng_.below(i) << 12) : (v & ~0x70u) | (2u << 4);
      }
      xf(XF_TEXGEN + i, v);
      xf(XF_POST_TEXGEN + i, rng_.below(64) | (rng_.below(2) << 8));
    }
  }

  void random_draw() {
    // Vertex descriptor: matrix indices sometimes, a position always.
    u32 lo = rng_.chance(20) ? static_cast<u32>(rng_.next()) & 0x1FF : 0;
    lo |= (1 + rng_.below(3)) << 9;                         // position
    lo |= (rng_.chance(40) ? rng_.below(4) : 0) << 11;      // normal
    lo |= (rng_.chance(60) ? 1 + rng_.below(3) : 0) << 13;  // colour 0
    lo |= (rng_.chance(15) ? 1 + rng_.below(3) : 0) << 15;  // colour 1
    u32 hi = 0;
    for (u32 i = 0; i < 8; ++i)
      if (rng_.chance(i == 0 ? 75 : 12)) hi |= (1 + rng_.below(3)) << (2 * i);
    const u8 vat = static_cast<u8>(rng_.below(8));
    auto fmt = [&](u32 max) { return rng_.below(max); };
    // VAT A: position (count, format, frac), normal (NBT, format, index3), colours, texcoord 0.
    u32 a = rng_.below(2) | (fmt(5) << 1) | (rng_.below(16) << 4);
    a |= (rng_.below(2) << 9) | ((rng_.chance(50) ? 4u : 1 + 2 * rng_.below(2)) << 10);
    a |= fmt(6) << 14 | fmt(6) << 18;
    a |= rng_.below(2) << 21 | fmt(5) << 22 | rng_.below(16) << 25;
    a |= rng_.below(2) << 31;
    u32 b = 0, c = 0;
    const u32 bfields[4] = {0, 9, 18, 27};
    for (u32 sh : bfields) b |= (rng_.below(2) | fmt(5) << 1) << sh;
    for (u32 sh : {4u, 13u, 22u}) b |= rng_.below(16) << sh;
    for (u32 sh : {5u, 14u, 23u}) c |= (rng_.below(2) | fmt(5) << 1) << sh;
    for (u32 sh : {0u, 9u, 18u, 27u}) c |= rng_.below(16) << sh;
    if (game_like_ && rng_.chance(70)) {
      // GP 2's usual layout: indexed or direct float positions, an RGBA8
      // colour 0 (or none), a float texture coordinate 0 (or none).
      lo = ((1 + rng_.below(3)) << 9) | ((rng_.chance(80) ? 1 + rng_.below(3) : 0) << 13);
      hi = rng_.chance(85) ? 1 + rng_.below(3) : 0;
      a = (a & ~0x1FFu) | 1 | (4u << 1) | (rng_.below(16) << 4);  // position: xyz, float
      a = (a & ~(7u << 14)) | ((5 + rng_.below(3)) << 14);         // colour 0: RGBA8
      a = (a & ~(0xFu << 21)) | (1u << 21) | (4u << 22);           // texture coordinate 0: st, float
    }
    cp_[0x50] = lo;
    cp_[0x60] = hi;
    cp_[0x70 + vat] = a;
    cp_[0x80 + vat] = b;
    cp_[0x90 + vat] = c;

    const u8 primitive = static_cast<u8>(rng_.below(8));
    const u16 count = static_cast<u16>(1 + rng_.below(48));
    stream_.clear();
    for (u32 v = 0; v < count; ++v) write_vertex(lo, hi, a, b, c);
    frontend_.draw(primitive, vat, count, stream_, cp_);
  }

  void write_index(u32 type, bool position) {
    const u32 max = type == 2 ? 255 : 4095;
    u32 index = rng_.below(max + 1);
    if (position && rng_.chance(2)) index = type == 2 ? 0xFF : 0xFFFF;  // "skip"
    if (type == 3) stream_.push_back(static_cast<u8>(index >> 8));
    stream_.push_back(static_cast<u8>(index));
  }
  void write_direct(u32 components, u32 format) {
    for (u32 k = 0; k < components; ++k) {
      if (format >= 4) {
        const u32 v = std::bit_cast<u32>(rng_.real(-64, 64));
        for (u32 i = 0; i < 4; ++i) stream_.push_back(static_cast<u8>(v >> (24 - 8 * i)));
      } else {
        for (u32 i = 0; i < kComponentSize[format]; ++i) stream_.push_back(static_cast<u8>(rng_.next()));
      }
    }
  }
  static u32 bits(u32 v, u32 shift, u32 n) { return (v >> shift) & ((1u << n) - 1); }

  void write_vertex(u32 lo, u32 hi, u32 a, u32 b, u32 c) {
    for (u32 i = 0; i < 9; ++i)
      if (lo & (1u << i)) stream_.push_back(static_cast<u8>(rng_.below(64)));
    const u32 pos = bits(lo, 9, 2);
    if (pos == 1) write_direct(bits(a, 0, 1) ? 3 : 2, bits(a, 1, 3));
    else write_index(pos, true);
    if (const u32 n = bits(lo, 11, 2); n == 1) {
      write_direct(bits(a, 9, 1) ? 9 : 3, bits(a, 10, 3));
    } else if (n) {
      const u32 indices = bits(a, 9, 1) && bits(a, 31, 1) ? 3 : 1;
      for (u32 k = 0; k < indices; ++k) write_index(n, false);
    }
    for (u32 i = 0; i < 2; ++i) {
      const u32 t = bits(lo, 13 + 2 * i, 2);
      if (t == 1) {
        for (u32 k = 0; k < kColorSize[bits(a, 14 + 4 * i, 3)]; ++k) stream_.push_back(static_cast<u8>(rng_.next()));
      } else if (t) {
        write_index(t, false);
      }
    }
    struct Field {
      u32 word, shift;
    };
    const Field tex[8] = {{a, 21}, {b, 0}, {b, 9}, {b, 18}, {b, 27}, {c, 5}, {c, 14}, {c, 23}};
    for (u32 i = 0; i < 8; ++i) {
      const u32 t = bits(hi, 2 * i, 2);
      if (t == 1) write_direct(bits(tex[i].word, tex[i].shift, 1) ? 2 : 1, bits(tex[i].word, tex[i].shift + 1, 3));
      else if (t) write_index(t, false);
    }
  }

  Rng rng_;
  std::vector<u8> ram_;
  u32 cp_[0x100] = {};
  Output output_;
  Frontend frontend_{read, this};
  std::vector<u8> stream_;
  bool game_like_ = false;
  u64 vertices_ = 0;
};

}  // namespace

TEST_CASE("the vertex pipeline's output does not change") {
#if defined(__x86_64__) || defined(_M_X64)
  Test t(20261015);
  u64 h[6];
  h[0] = t.frame(600, 0);    // 4:3
  h[1] = t.frame(600, 107);  // 21:9
  h[2] = t.frame(600, 0);
  h[3] = t.frame(600, 249);
  h[4] = t.frame(600, 0, true);  // mostly the game's usual vertex layout
  h[5] = t.frame(600, 107, true);
  CHECK(t.vertices() > 20000);
  CHECK(h[0] == 0xE000BF8D6FC61D45ull);
  CHECK(h[1] == 0x59003377BFFFE4D6ull);
  CHECK(h[2] == 0xAA71767EAA8654E9ull);
  CHECK(h[3] == 0x5B04D55130FBA291ull);
  CHECK(h[4] == 0x344353EA177BDB73ull);
  CHECK(h[5] == 0x8377997B4BF43976ull);
#else
  MESSAGE("bit-exact values are for x86-64 only");
#endif
}

// Not run by default: test_vertex_pipeline -tc=benchmark --no-skip
TEST_CASE("benchmark" * doctest::skip()) {
  Test t(7);
  std::printf("%.1f ns per vertex\n", t.benchmark(4000, 30));
  Test t2(7);
  std::printf("  strips of 5-7 vertices only: %.1f ns\n", t2.benchmark(4000, 30, 100));
  Test t3(7);
  std::printf("  strips of 20-59 vertices only: %.1f ns\n", t3.benchmark(4000, 30, 0));
}
