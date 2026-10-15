// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/frontend.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#include "gx/texture.h"
#include "os/activity.h"

namespace gx {

namespace {

u32 be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }

// XXH64 (Yann Collet, BSD-2-Clause), for texture contents.
u64 hash_bytes(const u8* p, std::size_t n, u64 seed = 0) {
  constexpr u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull,
                P4 = 0x85EBCA77C2B2AE63ull, P5 = 0x27D4EB2F165667C5ull;
  auto read64 = [](const u8* q) {
    u64 v;
    std::memcpy(&v, q, 8);
    return v;
  };
  auto round = [](u64 acc, u64 in) { return std::rotl(acc + in * P2, 31) * P1; };
  auto merge = [&](u64 acc, u64 v) { return (acc ^ round(0, v)) * P1 + P4; };
  const u8* end = p + n;
  u64 h;
  if (n >= 32) {
    u64 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    for (; p + 32 <= end; p += 32) {
      v1 = round(v1, read64(p));
      v2 = round(v2, read64(p + 8));
      v3 = round(v3, read64(p + 16));
      v4 = round(v4, read64(p + 24));
    }
    h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    h = merge(merge(merge(merge(h, v1), v2), v3), v4);
  } else {
    h = seed + P5;
  }
  h += n;
  for (; p + 8 <= end; p += 8) h = std::rotl(h ^ round(0, read64(p)), 27) * P1 + P4;
  if (p + 4 <= end) {
    u32 v;
    std::memcpy(&v, p, 4);
    h = std::rotl(h ^ (u64{v} * P1), 23) * P2 + P3;
    p += 4;
  }
  for (; p < end; ++p) h = std::rotl(h ^ (*p * P5), 11) * P1;
  h ^= h >> 33;
  h *= P2;
  h ^= h >> 29;
  h *= P3;
  h ^= h >> 32;
  return h;
}

// The clock for the per-draw timers: the CPU's time-stamp counter where there
// is one (invariant on the x86-64 CPUs OpenGP runs on), converted to ns at a
// rate measured against steady_clock. Reading steady_clock (on Windows,
// QueryPerformanceCounter) four times a draw cost about 0.5 ms a frame in the
// busiest races.
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
inline u64 ticks() { return __rdtsc(); }
#else
inline u64 ticks() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif

struct TickRate {
  u64 tick0 = 0;
  std::chrono::steady_clock::time_point time0;
  std::atomic<double> ns_per_tick{1.0};

  // Measures the rate since `tick0`.
  void measure() {
    const u64 t = ticks();
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - time0).count();
    if (t > tick0 && ns > 0) ns_per_tick.store(ns / static_cast<double>(t - tick0), std::memory_order_relaxed);
  }
};

TickRate& tick_rate() {
  static TickRate* rate = [] {
    auto* r = new TickRate;
    r->tick0 = ticks();
    r->time0 = std::chrono::steady_clock::now();
    // A first estimate over a millisecond; end_frame() refines it over the run.
    while (std::chrono::steady_clock::now() - r->time0 < std::chrono::milliseconds(1)) {}
    r->measure();
    return r;
  }();
  return *rate;
}

u64 ticks_to_ns(u64 t) {
  return static_cast<u64>(static_cast<double>(t) * tick_rate().ns_per_tick.load(std::memory_order_relaxed));
}

// Adds to a counter that only the thread running the game writes (guest
// threads take turns), so it needs no locked add.
void add(std::atomic<u64>& counter, u64 v) {
  counter.store(counter.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

// Adds the time from construction to destruction to a counter (ns).
class ScopeTimer {
 public:
  explicit ScopeTimer(std::atomic<u64>& total) : total_(total), start_(ticks()) {}
  ~ScopeTimer() { add(total_, ticks_to_ns(ticks() - start_)); }

 private:
  std::atomic<u64>& total_;
  u64 start_;
};

// A draw's time, and the part of it from the transform's start: three clock
// reads a draw.
class DrawTimer {
 public:
  explicit DrawTimer(Frontend::Stats& stats) : stats_(stats), start_(ticks()) {}
  ~DrawTimer() {
    const u64 end = ticks();
    add(stats_.draw_ns, ticks_to_ns(end - start_));
    if (transform_) add(stats_.transform_ns, ticks_to_ns(end - transform_));
  }
  void transform_starts() { transform_ = ticks(); }

 private:
  Frontend::Stats& stats_;
  u64 start_, transform_ = 0;
};

u64 mix(u64 h, u64 v) { return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0xBF58476D1CE4E5B9ull; }

// Alpha test: can it fail, pass, or depends on the pixel? (Dolphin AlphaTest::TestResult)
enum class AlphaTestResult { Undetermined, Fail, Pass };
AlphaTestResult alpha_test_result(u32 v) {
  const u32 c0 = bits(v, 16, 3), c1 = bits(v, 19, 3);
  constexpr u32 NEVER = 0, ALWAYS = 7;
  switch (bits(v, 22, 2)) {
    case 0:  // and
      if (c0 == ALWAYS && c1 == ALWAYS) return AlphaTestResult::Pass;
      if (c0 == NEVER || c1 == NEVER) return AlphaTestResult::Fail;
      break;
    case 1:  // or
      if (c0 == ALWAYS || c1 == ALWAYS) return AlphaTestResult::Pass;
      if (c0 == NEVER && c1 == NEVER) return AlphaTestResult::Fail;
      break;
    case 2:  // xor
      if ((c0 == ALWAYS && c1 == NEVER) || (c0 == NEVER && c1 == ALWAYS)) return AlphaTestResult::Pass;
      if ((c0 == ALWAYS && c1 == ALWAYS) || (c0 == NEVER && c1 == NEVER)) return AlphaTestResult::Fail;
      break;
    default:  // xnor
      if ((c0 == ALWAYS && c1 == NEVER) || (c0 == NEVER && c1 == ALWAYS)) return AlphaTestResult::Fail;
      if ((c0 == ALWAYS && c1 == ALWAYS) || (c0 == NEVER && c1 == NEVER)) return AlphaTestResult::Pass;
      break;
  }
  return AlphaTestResult::Undetermined;
}

// 11-bit fog float (sign, 8-bit exponent, 11-bit mantissa) to float.
float fog_float(u32 sign, u32 exp, u32 mant) { return std::bit_cast<float>((sign << 31) | (exp << 23) | (mant << 12)); }

}  // namespace

// Maps GX clip space to host clip space for the current viewport and
// scissor offset: x/y in EFB pixels become host NDC (+y up), z becomes the
// EFB depth (0..1). Linear in the clip coordinates, so the host still
// clips (and interpolates) correctly. With widescreen, the host's NDC span
// the wide EFB (x0 includes the added side).
struct Frontend::Viewport {
  float wd, ht, zrange, farz, x0, y0;
  float efb_width;
  void apply(const float* clip, float* out) const {
    const float w = clip[3];
    out[0] = (clip[0] * wd + x0 * w) * (2.0f / efb_width) - w;
    out[1] = w - (clip[1] * ht + y0 * w) * (2.0f / kEfbHeight);
    out[2] = (clip[2] * zrange + farz * w) * (1.0f / 16777216.0f);
    out[3] = w;
  }
};

Frontend::Frontend(MemoryReader read, void* context) : read_(read), context_(context), tmem_(kTmemSize, 0) {
  if (const char* env = std::getenv("OPENGP_GX_DUMP")) dump_frame_ = std::strtoull(env, nullptr, 10);
  if (const char* env = std::getenv("OPENGP_EFB_SYNC")) efb_sync_ = *env && *env != '0';
  bp_[BP_GENMODE] = 0;
  bp_[BP_ZMODE] = 0x17;  // test and update enabled, LEQUAL
  bp_[BP_BLENDMODE] = 0x18;  // colour and alpha update
}

Frontend::~Frontend() = default;

Frame& Frontend::frame() {
  if (!frame_) {
    frame_ = output_ ? output_->recycle() : nullptr;
    if (frame_) frame_->clear();
    else frame_ = std::make_unique<Frame>();
    // The EFB's width only changes between displayed frames.
    if (new_display_frame_) side_ = std::min(requested_side_.load(std::memory_order_relaxed), 2048u);
    new_display_frame_ = false;
    frame_->efb_width = efb_width();
    // New frame: state and texture indices are per frame, and textures are
    // re-checked against memory at least once per frame.
    state_dirty_ = true;
    textures_dirty_ = true;
    dumping_ = frames_ == dump_frame_;
    run_.active = false;
    dumped_state_ = ~0u;
    if (dumping_) std::fprintf(stderr, "[gx] ---- frame %llu ----\n", static_cast<unsigned long long>(frames_));
  }
  return *frame_;
}

void Frontend::end_frame(bool present) {
  Frame& f = frame();
  f.number = frames_;
  if (present) {
    ++frames_;
    new_display_frame_ = true;
    peeks_[0].requested = peeks_[1].requested = false;
    if (frames_ % 600 == 0) tick_rate().measure();
    // Evict textures not used for a while.
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (it->second.last_used + 120 < frames_) {
        f.releases.push_back(it->second.handle);
        it = textures_.erase(it);
      } else {
        ++it;
      }
    }
    // Release EFB copies whose memory the game has written over since: a
    // texture lookup there no longer finds them (bind_texture), so they are
    // dead. GP 2 makes its shadow light map at a new address each race (64 MB
    // at 8x). A copy that is merely not sampled for a while stays.
    if (frames_ % 60 == 0) {
      for (auto it = copies_.begin(); it != copies_.end();) {
        const CopiedTexture& c = it->second;
        bool dead = false;
        if (!c.xfb && c.last_used + 300 < frames_) {
          const u32 size = texture_level_size(c.format, c.width, c.height);
          const u8* mem = read_(context_, it->first, size);
          dead = !mem || hash_bytes(mem, size) != c.memory_hash;
        }
        if (dead) {
          f.releases.push_back(c.handle);
          it = copies_.erase(it);
        } else {
          ++it;
        }
      }
    }
  } else {
    f.xfb = 0;
  }
  if (output_) output_->submit(std::move(frame_));
  frame_.reset();
}

// ---- CPU access to the EFB -----------------------------------------------------------------------

u32 Frontend::efb_read(u32 address, u32 alpha_mode) {
  const u32 x = (address & 0xFFF) >> 2, y = (address >> 12) & 0x3FF;
  if (!output_ || x >= kEfbWidth || y >= kEfbHeight) return 0;
  const bool depth = address & 0x00400000;
  EfbPeeks& peeks = peeks_[depth ? 1 : 0];
  auto request = [&](bool urgent) {
    auto r = std::make_shared<EfbReadback>(depth, urgent);
    Frame& f = frame();
    f.readbacks.push_back(r);
    f.commands.push_back({Command::Type::Readback, static_cast<u32>(f.readbacks.size() - 1)});
    stats_.readbacks.fetch_add(1, std::memory_order_relaxed);
    return r;
  };
  if (efb_sync_) {
    // Render everything so far, then read the whole EFB back (cached until
    // the EFB changes).
    if (!peeks.current) {
      auto r = request(true);
      end_frame(false);
      const ScopeTimer readback_timer(stats_.readback_ns);
      os::g_guest_wait.set("waiting for a readback of the picture");
      r->wait();
      os::g_guest_wait.set(nullptr);
      peeks.current = std::move(r);
    }
  } else {
    // Read back once per frame, at the first peek, without waiting; peeks
    // see the newest readback that has completed (usually last frame's).
    if (!peeks.requested) {
      peeks.requested = true;
      peeks.pending.push_back(request(false));
    }
    while (!peeks.pending.empty() && peeks.pending.front()->ready()) {
      peeks.current = std::move(peeks.pending.front());
      peeks.pending.pop_front();
    }
    if (!peeks.current) return 0;
  }
  const std::vector<u32>& pixels = peeks.current->pixels;
  if (pixels.size() != std::size_t{kEfbWidth} * kEfbHeight) return 0;
  const u32 v = pixels[y * kEfbWidth + x];
  if (depth) return v & 0xFFFFFF;
  // RGBA8 (red lowest) to what the game reads: ARGB.
  u32 color = (v & 0xFF00FF00) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
  if (pixel_format() != PixelFormat::RGBA6_Z24) color |= 0xFF000000;
  if (alpha_mode == 0) color &= 0x00FFFFFF;
  else if (alpha_mode == 1) color |= 0xFF000000;
  return color;
}

void Frontend::efb_write(u32 address, u32 value) {
  static bool warned = false;
  if (!warned) std::fprintf(stderr, "[gx] EFB writes are not supported (%08X = %08X)\n", address, value);
  warned = true;
}

// ---- Register writes -------------------------------------------------------------------------

void Frontend::bp_write(u8 reg, u32 value) {
  if (reg == BP_MASK) {
    bp_mask_ = value & 0xFFFFFF;
    return;
  }
  const u32 v = (bp_[reg] & ~bp_mask_) | (value & bp_mask_);
  bp_mask_ = 0xFFFFFF;
  bp_[reg] = v;
  state_dirty_ = true;

  if (reg >= 0x80 && reg < 0xC0) textures_dirty_ = true;
  switch (reg) {
    case BP_TRIGGER_EFB_COPY: efb_copy(v); break;
    case BP_LOAD_TLUT1: load_tlut(); break;
    case BP_PRELOAD_MODE:
      if (v) preload_tmem();
      break;
    case BP_TEX_INVALIDATE: textures_dirty_ = true; break;
    case BP_TEV_REGISTER_L:
    case BP_TEV_REGISTER_L + 2:
    case BP_TEV_REGISTER_L + 4:
    case BP_TEV_REGISTER_L + 6: {
      s32* r = bits(v, 23, 1) ? tev_konst_[(reg - BP_TEV_REGISTER_L) / 2] : tev_colors_[(reg - BP_TEV_REGISTER_L) / 2];
      r[0] = sbits(v, 0, 11);   // red
      r[3] = sbits(v, 12, 11);  // alpha
      break;
    }
    case BP_TEV_REGISTER_H:
    case BP_TEV_REGISTER_H + 2:
    case BP_TEV_REGISTER_H + 4:
    case BP_TEV_REGISTER_H + 6: {
      s32* r = bits(v, 23, 1) ? tev_konst_[(reg - BP_TEV_REGISTER_H) / 2] : tev_colors_[(reg - BP_TEV_REGISTER_H) / 2];
      r[2] = sbits(v, 0, 11);   // blue
      r[1] = sbits(v, 12, 11);  // green
      break;
    }
    default: break;
  }
}

void Frontend::xf_write(u16 address, std::span<const u8> words) {
  for (std::size_t i = 0; i + 4 <= words.size(); i += 4) {
    const u32 addr = address + static_cast<u32>(i / 4);
    xf_.write(static_cast<u16>(addr), be32(words.data() + i));
    if (addr >= XF_REGISTERS) state_dirty_ = true;  // viewport (fog range) etc.
  }
}

void Frontend::indexed_load(u8 opcode, u32 value, const u32* cp) {
  // Loads XF memory from one of the CP arrays 12-15 (A-D).
  const u32 array = 12 + (opcode - 0x20) / 8;
  const u32 index = value >> 16;
  const u32 count = bits(value, 12, 4) + 1;
  const u32 address = value & 0xFFF;
  const u32 base = cp[0xA0 + array] & 0x03FFFFFF, stride = cp[0xB0 + array] & 0xFF;
  const u8* src = read_(context_, base + index * stride, count * 4);
  if (!src) return;
  for (u32 i = 0; i < count; ++i) xf_.write(static_cast<u16>(address + i), be32(src + 4 * i));
}

void Frontend::load_tlut() {
  // Copies a palette from memory into TMEM.
  const u32 src = (bp_[BP_LOAD_TLUT0] << 5) & 0x01FFFFFF;
  const u32 dst = bits(bp_[BP_LOAD_TLUT1], 0, 10) << 9;
  const u32 size = bits(bp_[BP_LOAD_TLUT1], 10, 11) * 32;
  if (dst + size > kTmemSize) return;
  if (const u8* p = read_(context_, src, size)) std::memcpy(tmem_.data() + dst, p, size);
  textures_dirty_ = true;
}

void Frontend::preload_tmem() {
  // GXPreLoadEntireTexture: copies a texture into TMEM ahead of use.
  const u32 src = (bp_[BP_PRELOAD_ADDR] << 5) & 0x01FFFFFF;
  const u32 even = bits(bp_[BP_PRELOAD_TMEM_EVEN], 0, 15) * 32;
  const u32 count = bits(bp_[BP_PRELOAD_MODE], 0, 15);
  const u32 type = bits(bp_[BP_PRELOAD_MODE], 15, 2);
  if (type == 3) {
    // RGBA8: alpha/red and green/blue halves of each tile go to different banks.
    const u32 odd = bits(bp_[BP_PRELOAD_TMEM_ODD], 0, 15) * 32;
    const u8* p = read_(context_, src, count * 64);
    if (!p) return;
    for (u32 i = 0; i < count && even + 32 * (i + 1) <= kTmemSize && odd + 32 * (i + 1) <= kTmemSize; ++i) {
      std::memcpy(tmem_.data() + even + 32 * i, p + 64 * i, 32);
      std::memcpy(tmem_.data() + odd + 32 * i, p + 64 * i + 32, 32);
    }
  } else {
    const u32 size = std::min(count * 32, even < kTmemSize ? kTmemSize - even : 0);
    if (const u8* p = read_(context_, src, size)) std::memcpy(tmem_.data() + even, p, size);
  }
  textures_dirty_ = true;
}

// ---- EFB copies and clears ---------------------------------------------------------------------

void Frontend::efb_copy(u32 trigger) {
  const u32 tl = bp_[BP_EFB_TL], wh = bp_[BP_EFB_WH];
  u32 x = bits(tl, 0, 10), y = bits(tl, 10, 10);
  u32 w = bits(wh, 0, 10) + 1, h = bits(wh, 10, 10) + 1;
  // Oversized copies are clamped to the EFB (see Dolphin's BPStructs.cpp).
  if (x >= kEfbWidth || y >= kEfbHeight) return;
  w = std::min(w, kEfbWidth - x);
  h = std::min(h, kEfbHeight - y);

  if (output_) {
    Frame& f = frame();
    const u32 dest = (bp_[BP_EFB_ADDR] << 5) & 0x01FFFFFF;
    // Widescreen: a copy of the whole width copies the wide EFB, and its
    // texture is as much wider.
    const bool wide = side_ && x == 0 && w == kEfbWidth;
    auto host_width = [&](u32 width) { return wide ? width * efb_width() / kEfbWidth : width; };
    CopyCmd c{};
    c.src_x = wide ? 0 : x + side_, c.src_y = y, c.src_width = host_width(w), c.src_height = h;
    c.depth = pixel_format() == PixelFormat::Z24;
    c.efb_has_alpha = pixel_format() == PixelFormat::RGBA6_Z24;
    c.clamp_top = copy_clamp_top(trigger);
    c.clamp_bottom = copy_clamp_bottom(trigger);
    static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
    c.gamma = kGamma[copy_gamma(trigger)];
    const u32 f0 = bp_[BP_COPY_FILTER0], f1 = bp_[BP_COPY_FILTER1];
    c.filter[0] = bits(f0, 0, 6) + bits(f0, 6, 6);
    c.filter[1] = bits(f0, 12, 6) + bits(f0, 18, 6) + bits(f1, 0, 6);
    c.filter[2] = bits(f1, 6, 6) + bits(f1, 12, 6);

    if (copy_to_xfb(trigger)) {
      const u32 yscale_reg = bits(bp_[BP_COPY_Y_SCALE], 0, 9);
      const float yscale = copy_scale_invert(trigger) ? 256.0f / static_cast<float>(std::max(yscale_reg, 1u))
                                                      : static_cast<float>(yscale_reg) / 256.0f;
      c.format = CopyFormat::XFB;
      const u32 width = w;
      c.height = std::max(1u, static_cast<u32>(1.0f + static_cast<float>(bits(wh, 10, 10)) * yscale));
      c.width = host_width(width);
      auto it = copies_.find(dest);
      if (it == copies_.end() || it->second.width != width || it->second.height != c.height) {
        if (it != copies_.end()) f.releases.push_back(it->second.handle);
        it = copies_.insert_or_assign(dest, CopiedTexture{new_handle(), width, c.height, TexFormat::RGBA8, 0, 0, true})
                 .first;
      }
      it->second.last_used = frames_;
      c.dest = it->second.handle;
      f.copies.push_back(c);
      f.commands.push_back({Command::Type::Copy, static_cast<u32>(f.copies.size() - 1)});
      f.xfb = c.dest;
      f.xfb_width = c.width;
      f.xfb_height = c.height;
      // The game's XFB is 4:3; a wide one is as much wider.
      f.xfb_aspect = 4.0f / 3.0f * static_cast<float>(c.width) / static_cast<float>(width);
    } else {
      c.format = copy_format(trigger);
      c.intensity = copy_intensity(trigger) && copy_auto_conv(trigger);
      const u32 width = copy_half_scale(trigger) ? std::max(1u, w / 2) : w;
      c.height = copy_half_scale(trigger) ? std::max(1u, h / 2) : h;
      c.width = host_width(width);
      const TexFormat tf = copy_texture_format(c.format);
      const u32 size = texture_level_size(tf, width, c.height);
      const u8* mem = read_(context_, dest, size);
      const u64 memory_hash = mem ? hash_bytes(mem, size) : 0;
      auto it = copies_.find(dest);
      if (it == copies_.end() || it->second.width != width || it->second.height != c.height) {
        if (it != copies_.end()) f.releases.push_back(it->second.handle);
        it = copies_.insert_or_assign(dest, CopiedTexture{new_handle(), width, c.height, tf, memory_hash, 0, false}).first;
      }
      it->second.format = tf;
      it->second.memory_hash = memory_hash;
      it->second.last_used = frames_;
      c.dest = it->second.handle;
      f.copies.push_back(c);
      f.commands.push_back({Command::Type::Copy, static_cast<u32>(f.copies.size() - 1)});
      textures_dirty_ = true;  // a texture at the destination now reads the copy
    }
  }

  if (dumping_)
    std::fprintf(stderr, "[gx]  copy %s %u,%u %ux%u to %08X format %u (trigger %06X, EFB format %u)%s\n",
                 copy_to_xfb(trigger) ? "XFB" : "texture", x, y, w, h, (bp_[BP_EFB_ADDR] << 5) & 0x01FFFFFF,
                 static_cast<u32>(copy_format(trigger)), trigger, static_cast<u32>(pixel_format()),
                 copy_clear(trigger) ? " + clear" : "");
  if (copy_clear(trigger)) clear(x, y, w, h);
  if (copy_to_xfb(trigger)) {
    static const bool log = std::getenv("OPENGP_FRAME_LOG") != nullptr;
    if (log) {
      static auto last = std::chrono::steady_clock::now();
      static u64 last_draw = 0, last_vertices = 0;
      const auto now = std::chrono::steady_clock::now();
      const u64 draw = stats_.draw_ns.load(), vertices = stats_.vertices.load();
      std::fprintf(stderr, "[frame] %llu: %.2f ms, draws %.2f ms, %llu vertices\n",
                   static_cast<unsigned long long>(frames_),
                   std::chrono::duration<double, std::milli>(now - last).count(), (draw - last_draw) * 1e-6,
                   static_cast<unsigned long long>(vertices - last_vertices));
      last = now, last_draw = draw, last_vertices = vertices;
    }
    end_frame(true);
  }
}

void Frontend::clear(u32 x, u32 y, u32 width, u32 height) {
  if (!output_) return;
  const PixelFormat fmt = pixel_format();
  ClearCmd c{};
  const bool wide = x == 0 && width == kEfbWidth;
  c.x = wide ? 0 : x + side_, c.y = y, c.width = wide ? efb_width() : width, c.height = height;
  c.color = bits(bp_[BP_BLENDMODE], 3, 1);
  c.alpha = bits(bp_[BP_BLENDMODE], 4, 1) && fmt == PixelFormat::RGBA6_Z24;
  c.depth = bits(bp_[BP_ZMODE], 4, 1);
  const u32 ar = bp_[BP_CLEAR_AR], gb = bp_[BP_CLEAR_GB];
  c.rgba = bits(ar, 0, 8) | (bits(gb, 8, 8) << 8) | (bits(gb, 0, 8) << 16) | (bits(ar, 8, 8) << 24);
  u32 z = bp_[BP_CLEAR_Z] & 0xFFFFFF;
  if (fmt == PixelFormat::RGB565_Z16) z = (z >> 8) * 0x10101 >> 8;  // 16-bit depth (Dolphin Z24ToZ16ToZ24)
  c.z = static_cast<float>(z) / 16777216.0f;
  if (!c.color && !c.alpha && !c.depth) return;
  efb_changed();
  Frame& f = frame();
  f.clears.push_back(c);
  f.commands.push_back({Command::Type::Clear, static_cast<u32>(f.clears.size() - 1)});
}

// ---- Draw state --------------------------------------------------------------------------------

PipelineKey Frontend::pipeline_key() const {
  const u32 blend = bp_[BP_BLENDMODE];
  const bool target_alpha = pixel_format() == PixelFormat::RGBA6_Z24;
  const bool may_pass = alpha_test_result(bp_[BP_ALPHA_COMPARE]) != AlphaTestResult::Fail;
  bool color_update = bits(blend, 3, 1) && may_pass;
  bool alpha_update = bits(blend, 4, 1) && target_alpha && may_pass;
  const bool dst_alpha = bits(bp_[BP_CONSTANT_ALPHA], 8, 1) && alpha_update;

  u32 enable = 0, src = 1, dst = 0, src_a = 1, dst_a = 0, sub = 0, sub_a = 0;
  if (bits(blend, 0, 1)) {
    enable = 1;
    if (bits(blend, 11, 1)) {  // subtract: dst - src
      sub = sub_a = 1;
      src = dst = src_a = dst_a = 1;
      if (dst_alpha) sub_a = 0, src_a = 1, dst_a = 0;
    } else {
      src = bits(blend, 8, 3);
      dst = bits(blend, 5, 3);
      if (!target_alpha) {  // no destination alpha: it reads as 1
        auto remove_dst_alpha = [](u32 f) { return f == 6 ? 1u : f == 7 ? 0u : f; };
        src = remove_dst_alpha(src);
        dst = remove_dst_alpha(dst);
      }
      // Colour factors on the alpha channel become the matching alpha factors.
      src_a = src == 2 ? 6 : src == 3 ? 7 : src;
      dst_a = dst == 2 ? 4 : dst == 3 ? 5 : dst;
      if (dst_alpha) src_a = 1, dst_a = 0;
    }
  } else if (bits(blend, 1, 1) && bits(blend, 12, 4) == 5) {  // logic op no-op
    color_update = false;
    alpha_update = alpha_update && dst_alpha;
  }
  if (!color_update) src = 0, dst = 1;
  if (!alpha_update) src_a = 0, dst_a = 1;

  const u32 z = bp_[BP_ZMODE];
  const bool early = bits(z, 0, 1) && bits(bp_[BP_ZCOMPARE], 6, 1) &&
                     alpha_test_result(bp_[BP_ALPHA_COMPARE]) == AlphaTestResult::Undetermined;
  PipelineKey k;
  k.bits = enable | (src << 1) | (dst << 4) | (src_a << 7) | (dst_a << 10) | (sub << 13) | (sub_a << 14) |
           (u32{color_update} << 15) | (u32{alpha_update} << 16) | (bits(z, 0, 1) << 17) | (bits(z, 1, 3) << 18) |
           (bits(z, 4, 1) << 21) | (u32{early} << 22);
  return k;
}

void Frontend::scissor(s32* rect) const {
  const u32 tl = bp_[BP_SCISSOR_TL], br = bp_[BP_SCISSOR_BR], off = bp_[BP_SCISSOR_OFFSET];
  const s32 x_off = static_cast<s32>(bits(off, 0, 9) << 1), y_off = static_cast<s32>(bits(off, 10, 9) << 1);
  const s32 left = std::clamp(static_cast<s32>(bits(tl, 12, 11)) - x_off, 0, static_cast<s32>(kEfbWidth));
  const s32 right = std::clamp(static_cast<s32>(bits(br, 12, 11)) - x_off + 1, 0, static_cast<s32>(kEfbWidth));
  const s32 top = std::clamp(static_cast<s32>(bits(tl, 0, 11)) - y_off, 0, static_cast<s32>(kEfbHeight));
  const s32 bottom = std::clamp(static_cast<s32>(bits(br, 0, 11)) - y_off + 1, 0, static_cast<s32>(kEfbHeight));
  rect[0] = left, rect[1] = top;
  rect[2] = std::max(0, right - left), rect[3] = std::max(0, bottom - top);
  // Into the wide EFB: a scissor over the whole width spans it.
  if (left == 0 && right == static_cast<s32>(kEfbWidth)) rect[2] = static_cast<s32>(efb_width());
  else rect[0] += static_cast<s32>(side_);
}

void Frontend::build_uniforms(PixelUniforms& u) const {
  std::memset(&u, 0, sizeof(u));
  std::memcpy(u.colors, tev_colors_, sizeof(u.colors));

  // Konst table (Dolphin's layout): 0-7 fixed fractions, 12-15 K0-K3 (rgb),
  // 16-31 single components of K0-K3 replicated (.rrrr, .gggg, .bbbb, .aaaa).
  static constexpr s32 kFractions[8] = {255, 223, 191, 159, 128, 96, 64, 32};
  for (u32 i = 0; i < 8; ++i)
    for (u32 c = 0; c < 4; ++c) u.konst[i][c] = kFractions[i];
  for (u32 k = 0; k < 4; ++k) {
    for (u32 c = 0; c < 3; ++c) u.konst[12 + k][c] = tev_konst_[k][c];
    for (u32 c = 0; c < 4; ++c)
      for (u32 j = 0; j < 4; ++j) u.konst[16 + c * 4 + k][j] = tev_konst_[k][c];
  }

  const u32 alpha = bp_[BP_ALPHA_COMPARE];
  u.alpha_ref[0] = static_cast<s32>(bits(alpha, 0, 8));
  u.alpha_ref[1] = static_cast<s32>(bits(alpha, 8, 8));

  for (u32 i = 0; i < 8; ++i) {
    u.texsize[i][0] = std::max(texsizes_[i][0], 1);
    u.texsize[i][1] = std::max(texsizes_[i][1], 1);
    u.tcscale[i][0] = static_cast<s32>(bits(bp_[BP_SU_SSIZE + 2 * i], 0, 16) + 1);
    u.tcscale[i][1] = static_cast<s32>(bits(bp_[BP_SU_TSIZE + 2 * i], 0, 16) + 1);
  }
  for (u32 i = 0; i < 2; ++i) {
    const u32 ss = bp_[BP_RAS1_SS0 + i];
    for (u32 c = 0; c < 4; ++c) u.indscale[i][c] = static_cast<s32>(bits(ss, 4 * c, 4));
  }
  for (u32 m = 0; m < 3; ++m) {
    const u32 a = bp_[BP_IND_MTXA + 3 * m], b = bp_[BP_IND_MTXB + 3 * m], c = bp_[BP_IND_MTXC + 3 * m];
    const s32 scale = static_cast<s32>(bits(a, 22, 2) | (bits(b, 22, 2) << 2) | (bits(c, 22, 1) << 4));
    const s32 row0[4] = {sbits(a, 0, 11), sbits(b, 0, 11), sbits(c, 0, 11), 17 - scale};
    const s32 row1[4] = {sbits(a, 11, 11), sbits(b, 11, 11), sbits(c, 11, 11), 17 - scale};
    std::memcpy(u.indmtx[2 * m], row0, sizeof(row0));
    std::memcpy(u.indmtx[2 * m + 1], row1, sizeof(row1));
  }

  // Fog.
  const u32 fc = bp_[BP_FOG_COLOR];
  u.fog_color[0] = static_cast<s32>(bits(fc, 16, 8));
  u.fog_color[1] = static_cast<s32>(bits(fc, 8, 8));
  u.fog_color[2] = static_cast<s32>(bits(fc, 0, 8));
  const u32 p0 = bp_[BP_FOG_PARAM0], p3 = bp_[BP_FOG_PARAM3];
  const bool nan_case = bits(p0, 11, 8) == 255 && bits(p3, 11, 8) == 255;
  constexpr float inf = std::numeric_limits<float>::infinity();
  u.fog_f[0] = nan_case ? 0.0f : fog_float(bits(p0, 19, 1), bits(p0, 11, 8), bits(p0, 0, 11));
  u.fog_f[1] = nan_case ? ((!bits(p0, 19, 1) && !bits(p3, 19, 1)) ? -inf : inf)
                        : fog_float(bits(p3, 19, 1), bits(p3, 11, 8), bits(p3, 0, 11));
  u.fog_i[1] = static_cast<s32>(bp_[BP_FOG_B_MAGNITUDE] & 0xFFFFFF);
  u.fog_i[3] = static_cast<s32>(bp_[BP_FOG_B_EXPONENT] & 0xFFFFFF);
  const u32 range = bp_[BP_FOG_RANGE];
  if (bits(range, 10, 1)) {
    const float wd = xf_.viewport_wd();
    const float center = static_cast<float>(static_cast<s32>(bits(range, 0, 10)) - 342);
    u.fog_f[2] = (center / (2.0f * wd)) * 2.0f - 1.0f;
    u.fog_f[3] = 2.0f * wd;
    for (u32 i = 0, v = 0; i < 5; ++i) {
      const u32 k = bp_[BP_FOG_RANGE + 1 + i];
      u.fog_range[v / 4][v % 4] = static_cast<float>(bits(k, 12, 12)) / 256.0f * 4.0f;
      ++v;
      u.fog_range[v / 4][v % 4] = static_cast<float>(bits(k, 0, 12)) / 256.0f * 4.0f;
      ++v;
    }
  } else {
    u.fog_f[2] = 0.0f;
    u.fog_f[3] = 1.0f;
  }

  // TEV stages and orders; the indirect stage table is indexed by indirect
  // stage and only filled for stages some TEV stage actually uses.
  const u32 genmode = bp_[BP_GENMODE];
  const u32 stages = genmode_num_tev_stages(genmode), ind_stages = genmode_num_ind_stages(genmode);
  for (u32 i = 0; i < 16; ++i) {
    u.stages[i][0] = bp_[BP_TEV_COLOR_ENV + 2 * i];
    u.stages[i][1] = bp_[BP_TEV_ALPHA_ENV + 2 * i];
    u.stages[i][2] = bp_[BP_IND_CMD + i] & 0x1FFFFF;
  }
  const u32 iref = bp_[BP_IREF];
  for (u32 i = 0; i < stages; ++i) {
    const u32 ind = bp_[BP_IND_CMD + i];
    const u32 bt = bits(ind, 0, 2);
    const bool active = bits(ind, 7, 2) != 0 || bits(ind, 9, 2) != 0;
    if (active && bt < ind_stages) u.stages[bt][3] = bits(iref, 6 * bt + 3, 3) | (bits(iref, 6 * bt, 3) << 8) | (1u << 16);
  }
  for (u32 i = 0; i < 8; ++i) {
    u.orders[i][0] = bp_[BP_TREF + i];
    u.orders[i][1] = bp_[BP_TEV_KSEL + i];
  }

  u.misc[0] = genmode;
  u.misc[1] = alpha;
  u.misc[2] = p3;
  u.misc[3] = range;

  const u32 blend = bp_[BP_BLENDMODE];
  const bool rgba6 = pixel_format() == PixelFormat::RGBA6_Z24;
  u.misc2[0] = (bits(blend, 4, 1) && bits(bp_[BP_CONSTANT_ALPHA], 8, 1) && rgba6) ? (bp_[BP_CONSTANT_ALPHA] & 0x1FF) : 0;
  u.misc2[1] = 0;  // rgba6 quantisation: off (true colour, as Dolphin's default)
  u.misc2[2] = 0;  // dithering: off with true colour
  const bool logic_op = !bits(blend, 0, 1) && bits(blend, 1, 1) && bits(blend, 12, 4) != 5;
  u.misc2[3] = logic_op ? (0x10 | bits(blend, 12, 4)) : 0;
}

u32 Frontend::draw_state() {
  Frame& f = frame();
  if (!state_dirty_ && !textures_dirty_ && state_index_ < f.states.size()) return state_index_;
  const ScopeTimer timer(stats_.state_ns);

  if (textures_dirty_) {
    // Texture maps the TEV (and indirect stages) read.
    const u32 genmode = bp_[BP_GENMODE];
    bool used[8] = {};
    for (u32 s = 0; s < genmode_num_tev_stages(genmode); ++s) {
      const u32 order = bp_[BP_TREF + s / 2] >> ((s & 1) ? 12 : 0);
      if (bits(order, 6, 1)) used[bits(order, 0, 3)] = true;
    }
    for (u32 i = 0; i < genmode_num_ind_stages(genmode); ++i) used[bits(bp_[BP_IREF], 6 * i, 3)] = true;
    for (u32 m = 0; m < 8; ++m) {
      if (used[m]) {
        state_.textures[m] = bind_texture(m, texsizes_[m]);
      } else {
        state_.textures[m] = {};
        texsizes_[m][0] = texsizes_[m][1] = 1;
      }
    }
    textures_dirty_ = false;
  }

  state_.pipeline = pipeline_key();
  scissor(state_.scissor);
  build_uniforms(uniforms_);
  if (f.uniforms.empty() || std::memcmp(&f.uniforms.back(), &uniforms_, sizeof(uniforms_)) != 0) f.uniforms.push_back(uniforms_);
  state_.uniforms = static_cast<u32>(f.uniforms.size() - 1);
  if (f.states.empty() || !(f.states.back() == state_)) f.states.push_back(state_);
  state_index_ = static_cast<u32>(f.states.size() - 1);
  state_dirty_ = false;
  return state_index_;
}

// ---- Textures ------------------------------------------------------------------------------------

TextureBinding Frontend::bind_texture(u32 map, s32* size) {
  const ScopeTimer timer(stats_.texture_ns);
  const u32 mode0 = bp_[tex_reg(BP_TX_SETMODE0, map)], mode1 = bp_[tex_reg(BP_TX_SETMODE1, map)];
  const u32 image0 = bp_[tex_reg(BP_TX_SETIMAGE0, map)], image1 = bp_[tex_reg(BP_TX_SETIMAGE1, map)];
  const u32 image3 = bp_[tex_reg(BP_TX_SETIMAGE3, map)], tlut = bp_[tex_reg(BP_TX_SETTLUT, map)];
  const u32 width = bits(image0, 0, 10) + 1, height = bits(image0, 10, 10) + 1;
  size[0] = static_cast<s32>(width);
  size[1] = static_cast<s32>(height);
  if (!is_valid_format(bits(image0, 20, 4))) return {};
  const TexFormat format = static_cast<TexFormat>(bits(image0, 20, 4));
  const u32 address = (bits(image3, 0, 24) << 5) & 0x01FFFFFF;

  u32 levels = 1;
  if (bits(mode0, 5, 2) != 0) {  // mipmapping on
    const u32 max_lod = (bits(mode1, 8, 8) + 0xF) / 0x10;
    levels = std::min<u32>(static_cast<u32>(std::bit_width(std::max(width, height))), max_lod + 1);
  }

  TextureBinding binding{0, mode0, mode1};
  const u32 bytes = texture_size(format, width, height, levels);
  const bool from_tmem = bits(image1, 21, 1);
  const u8* data = nullptr;
  if (from_tmem) {
    const u32 tmem = bits(image1, 0, 15) * 32;
    if (format == TexFormat::RGBA8) {
      static bool warned = false;
      if (!warned) std::fprintf(stderr, "[gx] preloaded RGBA8 textures are not supported\n");
      warned = true;
      return binding;
    }
    if (tmem + bytes <= kTmemSize) data = tmem_.data() + tmem;
  } else {
    data = read_(context_, address, bytes);
  }
  if (!data) return binding;

  const u64 data_hash = from_tmem ? hash_bytes(data, bytes) : memory_hash(data, address, bytes);

  // An EFB copy at this address, not overwritten since?
  if (!from_tmem) {
    if (auto it = copies_.find(address); it != copies_.end() && it->second.width == width && it->second.height == height) {
      const u32 copy_bytes = texture_level_size(it->second.format, width, height);
      const u8* mem = read_(context_, address, copy_bytes);
      if (mem && memory_hash(mem, address, copy_bytes) == it->second.memory_hash) {
        it->second.last_used = frames_;
        binding.texture = it->second.handle;
        return binding;
      }
    }
  }

  u64 key = mix(mix(mix(data_hash, address), (u64{width} << 32) | height), (u64{levels} << 8) | static_cast<u32>(format));
  const u32 tlut_offset = bits(tlut, 0, 10) << 9;
  const TlutFormat tlut_format = static_cast<TlutFormat>(bits(tlut, 10, 2));
  if (is_paletted(format)) {
    const u32 entries = palette_entries(format);
    const u32 tlut_bytes = std::min(entries * 2, kTmemSize - tlut_offset);
    key = mix(mix(key, hash_bytes(tmem_.data() + tlut_offset, tlut_bytes)), static_cast<u32>(tlut_format));
  }

  Frame& f = frame();
  if (auto it = textures_.find(key); it != textures_.end()) {
    it->second.last_used = frames_;
    binding.texture = it->second.handle;
    return binding;
  }

  // Decode every level to RGBA8 into the frame's upload data.
  TextureUpload up{new_handle(), width, height, levels, f.texels.size()};
  u32 texels = 0;
  for (u32 l = 0, w = width, h = height; l < levels; ++l, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) texels += w * h;
  f.texels.resize(f.texels.size() + std::size_t{texels} * 4);
  u32* out = reinterpret_cast<u32*>(f.texels.data() + up.offset);
  const u8* src = data;
  for (u32 l = 0, w = width, h = height; l < levels; ++l) {
    const u32 pw = (w + 7) & ~7u, ph = (h + 7) & ~7u;
    if (scratch_.size() < std::size_t{pw} * ph) scratch_.resize(std::size_t{pw} * ph);
    decode_texture(out, src, format, w, h, tmem_.data() + tlut_offset, tlut_format, scratch_);
    out += w * h;
    src += texture_level_size(format, w, h);
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  f.uploads.push_back(up);
  textures_.emplace(key, CachedTexture{up.handle, frames_});
  if (dumping_)
    std::fprintf(stderr, "[gx]  texture %u: %08X format %u %ux%u, %u levels%s\n", up.handle, address,
                 static_cast<u32>(format), width, height, levels, from_tmem ? " (TMEM)" : "");
  binding.texture = up.handle;
  return binding;
}

u64 Frontend::memory_hash(const u8* data, u32 address, u32 size) {
  if (memory_hashes_.size() > 16384) memory_hashes_.clear();
  MemoryHash& m = memory_hashes_[(u64{size} << 32) | address];
  if (m.frame != frames_ + 1) {
    m.hash = hash_bytes(data, size);
    m.frame = frames_ + 1;
  }
  return m.hash;
}

// ---- Draws ---------------------------------------------------------------------------------------

void Frontend::draw(u8 primitive, u8 vat, u16 count, std::span<const u8> vertices, const u32* cp) {
  if (!output_ || count == 0) return;
  DrawTimer timer(stats_);
  Frame& f = frame();

  const u32 state = draw_state();
  const s32* sc = f.states[state].scissor;
  if (sc[2] == 0 || sc[3] == 0) return;

  loader_.setup(cp, vat, ram_, ram_size_);
  const u32 stride = loader_.size();
  if (std::size_t{stride} * count > vertices.size()) return;

  const u32 off = bp_[BP_SCISSOR_OFFSET];
  const Viewport vp{xf_.viewport_wd(),
                    xf_.viewport_ht(),
                    xf_.viewport_zrange(),
                    xf_.viewport_farz(),
                    xf_.viewport_x() - static_cast<float>(bits(off, 0, 9) << 1) + static_cast<float>(side_),
                    xf_.viewport_y() - static_cast<float>(bits(off, 10, 9) << 1),
                    static_cast<float>(efb_width())};

  const u32 base = static_cast<u32>(f.vertices.size());
  f.vertices.resize(base + count);
  clip_.resize(count);
  skip_.resize(count);
  const u32 mia = xf_.reg(XF_MATRIX_INDEX_A), mib = xf_.reg(XF_MATRIX_INDEX_B);
  TransformedVertex t{};  // an emboss generator may read a later, not yet written coordinate
  timer.transform_starts();
  add(stats_.vertices, count);
  xf_.prepare();
  for (u32 i = 0; i < count; ++i) {
    loader_.load(vertices.data() + std::size_t{i} * stride, input_, mia, mib);
    xf_.transform(input_, t);
    Vertex& v = f.vertices[base + i];
    vp.apply(t.clip, v.pos);
    v.color[0] = t.color[0];
    v.color[1] = t.color[1];
    std::memcpy(v.tex, t.tex, sizeof(v.tex));
    std::memcpy(clip_[i].data(), t.clip, sizeof(t.clip));
    skip_[i] = input_.skip;
  }

  const u32 first = static_cast<u32>(f.indices.size());
  assemble(primitive, base, count, vp);
  if (side_ && f.indices.size() > first) {
    if (xf_.perspective()) {
      run_.active = false;
      widen_overlay(base, f.states[state]);
    } else if (!widen_2d(base)) {
      extend_edges(base);
    }
  }
  const u32 added = static_cast<u32>(f.indices.size()) - first;

  if (dumping_) {
    if (state != dumped_state_) {
      dumped_state_ = state;
      const DrawState& s = f.states[state];
      const PixelUniforms& u = f.uniforms[s.uniforms];
      const u32 genmode = bp_[BP_GENMODE];
      std::fprintf(stderr,
                   "[gx]  state %u: pipeline %06X scissor %d,%d %dx%d genmode %06X (%u stages, %u texgens, %u ind, "
                   "cull %u) xf texgens %u colors %u alpha test %06X zmode %02X blend %04X pe %02X\n",
                   state, s.pipeline.bits, s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3], genmode,
                   genmode_num_tev_stages(genmode), genmode_num_texgens(genmode), genmode_num_ind_stages(genmode),
                   genmode_cull_mode(genmode), xf_.num_texgens(), xf_.reg(XF_NUM_COLORS), bp_[BP_ALPHA_COMPARE],
                   bp_[BP_ZMODE], bp_[BP_BLENDMODE], bp_[BP_ZCOMPARE]);
      for (u32 i = 0; i < genmode_num_tev_stages(genmode); ++i)
        std::fprintf(stderr, "[gx]    stage %u: color %06X alpha %06X order %06X ksel %06X ind %06X\n", i,
                     u.stages[i][0], u.stages[i][1], (bp_[BP_TREF + i / 2] >> ((i & 1) ? 12 : 0)) & 0xFFF,
                     bp_[BP_TEV_KSEL + i / 2], u.stages[i][2]);
      std::fprintf(stderr, "[gx]    regs %d,%d,%d,%d / %d,%d,%d,%d / %d,%d,%d,%d / %d,%d,%d,%d\n", u.colors[0][0],
                   u.colors[0][1], u.colors[0][2], u.colors[0][3], u.colors[1][0], u.colors[1][1], u.colors[1][2],
                   u.colors[1][3], u.colors[2][0], u.colors[2][1], u.colors[2][2], u.colors[2][3], u.colors[3][0],
                   u.colors[3][1], u.colors[3][2], u.colors[3][3]);
      for (u32 m = 0; m < 8; ++m)
        if (s.textures[m].texture || texsizes_[m][0] > 1)
          std::fprintf(stderr, "[gx]    texmap %u: handle %u %dx%d mode %06X/%04X\n", m, s.textures[m].texture,
                       texsizes_[m][0], texsizes_[m][1], s.textures[m].mode0, s.textures[m].mode1);
    }
    const Vertex& v = f.vertices[base];
    const float w = v.pos[3] != 0.0f ? v.pos[3] : 1.0f;
    // The draw's extent in game pixels (after any widescreen stretch).
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (std::size_t i = base; i < f.vertices.size(); ++i) {
      const float vw = f.vertices[i].pos[3] != 0.0f ? f.vertices[i].pos[3] : 1.0f;
      lo = std::min(lo, f.vertices[i].pos[0] / vw);
      hi = std::max(hi, f.vertices[i].pos[0] / vw);
    }
    const float half = static_cast<float>(efb_width()) * 0.5f, side = static_cast<float>(side_);
    std::fprintf(stderr,
                 "[gx]  draw prim %u vat %u count %u -> %u indices; %s x %.1f..%.1f; v0 ndc %.3f,%.3f z %.5f w %.3f "
                 "color %08X/%08X tex0 %.3f,%.3f,%.3f\n",
                 primitive, vat, count, added, xf_.perspective() ? "persp" : "ortho", (lo + 1.0f) * half - side,
                 (hi + 1.0f) * half - side, v.pos[0] / w, v.pos[1] / w, v.pos[2] / w, v.pos[3], v.color[0], v.color[1],
                 v.tex[0][0], v.tex[0][1], v.tex[0][2]);
  }
  if (added == 0) return;
  efb_changed();

  // Extend the previous draw if nothing else happened in between.
  if (!f.commands.empty() && f.commands.back().type == Command::Type::Draw) {
    DrawCmd& last = f.draws[f.commands.back().index];
    if (last.state == state && last.first_index + last.index_count == first) {
      last.index_count += added;
      return;
    }
  }
  f.draws.push_back({state, first, added});
  f.commands.push_back({Command::Type::Draw, static_cast<u32>(f.draws.size() - 1)});
}

// Widescreen, for 2D (orthographic) draws: 2D that spans the game's whole
// width (a background, a fade) is stretched over the wide EFB, unless it
// already covers it. Backgrounds are often drawn as a row of tiles, one draw
// each, so a run of draws of the same height, each starting where the last
// one ended, counts as one. Returns true if it stretched.
bool Frontend::widen_2d(u32 first_vertex) {
  Frame& f = frame();
  // Orthographic, so w is 1 and x/y are NDC.
  float x0 = std::numeric_limits<float>::max(), x1 = std::numeric_limits<float>::lowest();
  float y0 = x0, y1 = x1;
  for (std::size_t i = first_vertex; i < f.vertices.size(); ++i) {
    const float* p = f.vertices[i].pos;
    x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]);
    y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
  }
  // In game pixels.
  const float half_width = static_cast<float>(efb_width()) * 0.5f, side = static_cast<float>(side_);
  const float left = (x0 + 1.0f) * half_width - side, right = (x1 + 1.0f) * half_width - side;
  const float top = (1.0f - y1) * (kEfbHeight * 0.5f), bottom = (1.0f - y0) * (kEfbHeight * 0.5f);

  Run& run = run_;
  if (run.active && std::abs(left - run.right) <= 1.0f && std::abs(top - run.top) <= 1.0f &&
      std::abs(bottom - run.bottom) <= 1.0f) {
    run.right = std::max(run.right, right);
  } else {
    run = {true, first_vertex, left, right, top, bottom};
  }
  const float width = static_cast<float>(kEfbWidth);
  const bool spans = run.left <= 1.0f && run.right >= width - 1.0f;
  const bool covers_wide = run.left <= -side && run.right >= width + side;
  if (!spans || covers_wide) return false;
  // Mapping the game's 640 pixels onto the whole wide EFB scales NDC x.
  // (Edges extended on the run's first draws end up off the sides.)
  const float scale = static_cast<float>(efb_width()) / width;
  for (std::size_t i = run.first_vertex; i < f.vertices.size(); ++i) f.vertices[i].pos[0] *= scale;
  run.active = false;
  return true;
}

// Widescreen, for other 2D draws: they keep their place, but one that
// reaches the game's left or right screen edge (a banner running off the
// side) is continued to the wide EFB's edge with its edge's texels, so that
// it does not end short of it. Each pair of neighbouring vertices on the
// edge gets a quad out to the wide edge, their attributes copied.
void Frontend::extend_edges(u32 first_vertex) {
  Frame& f = frame();
  const u32 end = static_cast<u32>(f.vertices.size());
  const float half_width = static_cast<float>(efb_width()) * 0.5f, side = static_cast<float>(side_);
  const float width = static_cast<float>(kEfbWidth);
  auto game_x = [&](const Vertex& v) { return (v.pos[0] + 1.0f) * half_width - side; };  // w is 1
  float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
  for (u32 i = first_vertex; i < end; ++i) {
    lo = std::min(lo, game_x(f.vertices[i]));
    hi = std::max(hi, game_x(f.vertices[i]));
  }
  auto extend = [&](float edge, float out_ndc) {
    std::vector<u32>& on_edge = scratch_;
    on_edge.clear();
    for (u32 i = first_vertex; i < end; ++i)
      if (std::abs(game_x(f.vertices[i]) - edge) < 1.0f) on_edge.push_back(i);
    if (on_edge.size() < 2) return;
    std::sort(on_edge.begin(), on_edge.end(), [&](u32 a, u32 b) { return f.vertices[a].pos[1] < f.vertices[b].pos[1]; });
    for (std::size_t k = 0; k + 1 < on_edge.size(); ++k) {
      const Vertex a = f.vertices[on_edge[k]], b = f.vertices[on_edge[k + 1]];
      if (b.pos[1] - a.pos[1] < 1e-4f) continue;
      Vertex a_out = a, b_out = b;
      a_out.pos[0] = b_out.pos[0] = out_ndc;
      const u32 q = static_cast<u32>(f.vertices.size());
      f.vertices.insert(f.vertices.end(), {a_out, a, b_out, b});
      f.indices.insert(f.indices.end(), {q, q + 1, q + 2, q + 1, q + 3, q + 2});
    }
  };
  if (lo <= 0.5f && lo > -side) extend(lo, -1.0f);
  if (hi >= width - 0.5f && hi < width + side) extend(hi, 1.0f);
}

// Widescreen, for 3D (perspective) draws: a full-screen overlay drawn with
// the 3D projection (a flat draw facing the camera, so every vertex has the
// same w) that spans exactly the game's width, such as GP 2's rain and the
// fade at the end of its attract mode's demo race, is widened to the wide EFB
// as 2D is. (Large particles near the camera also face it, but don't end at
// the game's screen edges.) If its textures repeat, its texture coordinates
// carry on across the added sides, so the pattern keeps its size; otherwise
// it is stretched.
void Frontend::widen_overlay(u32 first_vertex, const DrawState& state) {
  Frame& f = frame();
  const u32 end = static_cast<u32>(f.vertices.size());
  const float w = f.vertices[first_vertex].pos[3];
  if (!(w > 0.0f)) return;
  float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
  for (u32 i = first_vertex; i < end; ++i) {
    const float* p = f.vertices[i].pos;
    if (std::abs(p[3] - w) > w * 1e-5f) return;
    lo = std::min(lo, p[0] / w), hi = std::max(hi, p[0] / w);
  }
  // In game pixels.
  const float half_width = static_cast<float>(efb_width()) * 0.5f, side = static_cast<float>(side_);
  const float width = static_cast<float>(kEfbWidth);
  const float left = (lo + 1.0f) * half_width - side, right = (hi + 1.0f) * half_width - side;
  if (std::abs(left) > 1.0f || std::abs(right - width) > 1.0f) return;
  const float scale = static_cast<float>(efb_width()) / width;

  // Flat at one depth, the draw's texture coordinates are affine in its NDC
  // x and y: each one's slope in x, from the three vertices that span the
  // largest triangle, and checked against the others.
  bool carry = true;
  for (const TextureBinding& t : state.textures)
    if (t.texture && (t.mode0 & 3) == 0) carry = false;  // clamped in s
  const u32 texgens = xf_.num_texgens();
  float slope[8][3] = {};
  if (carry && texgens > 0) {
    auto ndc = [&](u32 i, u32 axis) { return f.vertices[i].pos[axis] / w; };
    u32 a = first_vertex, b = first_vertex, c = first_vertex;
    float best = 0.0f;
    for (u32 i = first_vertex; i < end; ++i) {
      const float dx = ndc(i, 0) - ndc(a, 0), dy = ndc(i, 1) - ndc(a, 1), d = dx * dx + dy * dy;
      if (d > best) best = d, b = i;
    }
    best = 0.0f;
    const float bx = ndc(b, 0) - ndc(a, 0), by = ndc(b, 1) - ndc(a, 1);
    for (u32 i = first_vertex; i < end; ++i) {
      const float d = std::abs(bx * (ndc(i, 1) - ndc(a, 1)) - by * (ndc(i, 0) - ndc(a, 0)));
      if (d > best) best = d, c = i;
    }
    const float cx = ndc(c, 0) - ndc(a, 0), cy = ndc(c, 1) - ndc(a, 1);
    const float det = bx * cy - cx * by;
    if (std::abs(det) < 1e-6f) {
      carry = false;
    } else {
      for (u32 t = 0; t < texgens && carry; ++t) {
        for (u32 k = 0; k < 3 && carry; ++k) {
          const float a0 = f.vertices[a].tex[t][k];
          const float vb = f.vertices[b].tex[t][k] - a0, vc = f.vertices[c].tex[t][k] - a0;
          const float sx = (vb * cy - vc * by) / det, sy = (bx * vc - cx * vb) / det;
          for (u32 i = first_vertex; i < end && carry; ++i) {
            const float expected = a0 + sx * (ndc(i, 0) - ndc(a, 0)) + sy * (ndc(i, 1) - ndc(a, 1));
            if (std::abs(f.vertices[i].tex[t][k] - expected) > 1e-3f * (1.0f + std::abs(expected))) carry = false;
          }
          slope[t][k] = sx;
        }
      }
    }
  }
  for (u32 i = first_vertex; i < end; ++i) {
    Vertex& v = f.vertices[i];
    if (carry) {
      const float dx = v.pos[0] / w * (scale - 1.0f);
      for (u32 t = 0; t < texgens; ++t)
        for (u32 k = 0; k < 3; ++k) v.tex[t][k] += slope[t][k] * dx;
    }
    v.pos[0] *= scale;
  }
}

void Frontend::assemble(u8 primitive, u32 base, u16 count, const Viewport& vp) {
  Frame& f = frame();
  const u32 cull = genmode_cull_mode(bp_[BP_GENMODE]);
  const bool flip = vp.ht > 0;  // a positive viewport height mirrors y, and with it the winding

  auto tri = [&](u32 a, u32 b, u32 c) {
    if (skip_[a] || skip_[b] || skip_[c]) return;
    if (cull != 0) {
      if (cull == 3) return;
      const float* p0 = clip_[a].data();
      const float* p1 = clip_[b].data();
      const float* p2 = clip_[c].data();
      // Homogeneous orientation test (Dolphin's Clipper::IsBackface). Note
      // the sense: GX "cull back" removes the triangles this calls front
      // facing (see Dolphin's Clipper::ProcessTriangle).
      const float nz = (p0[0] * p2[3] - p2[0] * p0[3]) * p1[1] + (p2[0] * p0[1] - p0[0] * p2[1]) * p1[3] +
                       (p2[1] * p0[3] - p0[1] * p2[3]) * p1[0];
      const bool back = (nz <= 0.0f) != flip;
      if ((cull == 1 && !back) || (cull == 2 && back)) return;
    }
    f.indices.insert(f.indices.end(), {base + a, base + b, base + c});
  };

  // Lines and points become quads of the configured width (in 1/6 pixels).
  const u32 lp = bp_[BP_LINE_POINT_WIDTH];
  auto quad_from = [&](u32 src, float dx, float dy, u32 pair_src, float pdx, float pdy) {
    // Appends 4 vertices: src +/- (dx, dy) and pair_src +/- (pdx, pdy) (NDC offsets, scaled by w).
    const u32 q = static_cast<u32>(f.vertices.size());
    const Vertex a = f.vertices[base + src], b = f.vertices[base + pair_src];
    Vertex v[4] = {a, a, b, b};
    v[0].pos[0] -= dx * a.pos[3], v[0].pos[1] -= dy * a.pos[3];
    v[1].pos[0] += dx * a.pos[3], v[1].pos[1] += dy * a.pos[3];
    v[2].pos[0] -= pdx * b.pos[3], v[2].pos[1] -= pdy * b.pos[3];
    v[3].pos[0] += pdx * b.pos[3], v[3].pos[1] += pdy * b.pos[3];
    f.vertices.insert(f.vertices.end(), v, v + 4);
    f.indices.insert(f.indices.end(), {q, q + 1, q + 2, q + 1, q + 3, q + 2});
  };
  auto line = [&](u32 a, u32 b) {
    if (skip_[a] || skip_[b]) return;
    const float width = static_cast<float>(bits(lp, 0, 8)) / 6.0f;
    const Vertex& va = f.vertices[base + a];
    const Vertex& vb = f.vertices[base + b];
    const float ax = va.pos[0] / va.pos[3], ay = va.pos[1] / va.pos[3];
    const float bx = vb.pos[0] / vb.pos[3], by = vb.pos[1] / vb.pos[3];
    // Expand perpendicular to the major axis (in pixels), half the width each side.
    const float px = std::abs(bx - ax) * vp.efb_width, py = std::abs(by - ay) * kEfbHeight;
    const float dx = py > px ? width / vp.efb_width : 0.0f;
    const float dy = py > px ? 0.0f : width / kEfbHeight;
    quad_from(a, dx, dy, b, dx, dy);
  };
  auto point = [&](u32 a) {
    if (skip_[a]) return;
    const float size = static_cast<float>(bits(lp, 8, 8)) / 6.0f;
    const float dx = size / vp.efb_width, dy = size / kEfbHeight;
    // Top edge (a - dy) and bottom edge (a + dy), each spanning -dx..+dx.
    const u32 q = static_cast<u32>(f.vertices.size());
    const Vertex v = f.vertices[base + a];
    Vertex c[4] = {v, v, v, v};
    const float w = v.pos[3];
    c[0].pos[0] -= dx * w, c[0].pos[1] += dy * w;
    c[1].pos[0] += dx * w, c[1].pos[1] += dy * w;
    c[2].pos[0] -= dx * w, c[2].pos[1] -= dy * w;
    c[3].pos[0] += dx * w, c[3].pos[1] -= dy * w;
    f.vertices.insert(f.vertices.end(), c, c + 4);
    f.indices.insert(f.indices.end(), {q, q + 1, q + 2, q + 1, q + 3, q + 2});
  };

  switch (primitive) {
    case 0:  // quads
    case 1:  // quads (2)
      for (u32 i = 0; i + 4 <= count; i += 4) {
        tri(i, i + 1, i + 2);
        tri(i, i + 2, i + 3);
      }
      break;
    case 2:  // triangles
      for (u32 i = 0; i + 3 <= count; i += 3) tri(i, i + 1, i + 2);
      break;
    case 3:  // triangle strip
      for (u32 i = 0; i + 3 <= count; ++i) {
        if (i & 1) tri(i, i + 2, i + 1);
        else tri(i, i + 1, i + 2);
      }
      break;
    case 4:  // triangle fan
      for (u32 i = 1; i + 2 <= count; ++i) tri(0, i, i + 1);
      break;
    case 5:  // lines
      for (u32 i = 0; i + 2 <= count; i += 2) line(i, i + 1);
      break;
    case 6:  // line strip
      for (u32 i = 0; i + 2 <= count; ++i) line(i, i + 1);
      break;
    case 7:  // points
      for (u32 i = 0; i < count; ++i) point(i);
      break;
    default: break;
  }
}

}  // namespace gx
