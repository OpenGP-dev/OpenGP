// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keeping linked cabinets in step; see link_pace.h.

#include "link_pace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "hw/hardware.h"
#include "ppc/memory.h"

namespace link_pace {

namespace {

using gekko::u32;
using gekko::u8;

// The phase lock: the queue to hold (frames, averaged), how quickly the
// average follows (per frame), and how hard the clock is pushed (rate per
// frame of error), at most kMaxNudge. Only proportional: a client ahead of
// the host waits for its frames with one queued, so the queue says nothing
// below 1 (an integral would wind up); a client behind sees it grow. So the
// client settles a little behind the host, its queue now and then at 2 (which
// the host's slack lets through), never at 3. A difference of 0.05% between
// the cabinets' clocks (far more than real clocks') settles at about 2 in 40
// frames.
constexpr double kTarget = 1.0;
constexpr double kSmoothing = 0.02;  // about a second
constexpr double kGain = 0.01;
constexpr double kMaxNudge = 0.002;

struct State {
  bool racing = false;
  double average = 1.0;  // the queue, averaged
  // The host's slack: what it last wrote into each cabinet's queue byte, to
  // tell a fresh value from its own.
  std::array<int, 4> written{-1, -1, -1, -1};
  // For the log.
  std::chrono::steady_clock::time_point next_log{};
  u32 frames = 0, depth_sum = 0, depth_max = 0, freezes = 0, waits = 0;
  gekko::u64 exchange_max_ns = 0;
};

State g_state;

bool log_on() {
  static const bool on = std::getenv("OPENGP_LOG_LINK_PACE") != nullptr;
  return on;
}

// OPENGP_LINK_PACE=0 turns both off (to compare, or if they ever misbehave).
bool pace_on() {
  static const bool on = [] {
    const char* v = std::getenv("OPENGP_LINK_PACE");
    return !v || *v != '0';
  }();
  return on;
}

void set_rate(double rate) { hw::g_link_clock_rate.store(rate, std::memory_order_relaxed); }

}  // namespace

void on_frame(u8* base, const Layout& l, gekko::u64 exchange_ns) {
  State& s = g_state;
  const u32 scene = ppc::load_be<u32>(base + l.scene);
  const bool racing = base[l.link_active] && scene >= l.race_first && scene <= l.race_last;
  const bool host = base[l.host_flag] == 1;
  if (!racing) {
    if (s.racing) set_rate(1.0);
    s = State{};
    return;
  }
  if (!s.racing) {
    s.racing = true;
    s.next_log = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  }

  u32 depth = 0;
  if (!host) {
    depth = (ppc::load_be<u32>(base + l.ring_write) - ppc::load_be<u32>(base + l.ring_read)) & l.ring_mask;
    s.average += (static_cast<double>(depth) - s.average) * kSmoothing;
    const double error = s.average - kTarget;  // > 0: behind the host, so faster
    if (pace_on()) set_rate(1.0 + std::clamp(error * kGain, -kMaxNudge, kMaxNudge));
  }

  if (!log_on()) return;
  ++s.frames;
  s.depth_sum += depth;
  s.depth_max = std::max(s.depth_max, depth);
  s.exchange_max_ns = std::max(s.exchange_max_ns, exchange_ns);
  if (exchange_ns > 4'000'000) ++s.waits;  // a quarter of a frame: waiting for the other cabinet
  if (const auto now = std::chrono::steady_clock::now(); now >= s.next_log) {
    s.next_log = now + std::chrono::seconds(1);
    if (host)
      std::fprintf(stderr, "[link] host: %u freezes in %u frames, %u waits (longest %.1f ms)\n", s.freezes, s.frames,
                   s.waits, static_cast<double>(s.exchange_max_ns) / 1e6);
    else
      std::fprintf(stderr, "[link] client: queue %.2f average (max %u), clock %.4f%%, %u waits (longest %.1f ms)\n",
                   s.frames ? static_cast<double>(s.depth_sum) / s.frames : 0.0, s.depth_max,
                   hw::g_link_clock_rate.load(std::memory_order_relaxed) * 100.0, s.waits,
                   static_cast<double>(s.exchange_max_ns) / 1e6);
    s.frames = s.depth_sum = s.depth_max = s.freezes = s.waits = 0;
    s.exchange_max_ns = 0;
  }
}

void on_queue_check(u8* base, u32 entry, u32 queued_at, int cabinet) {
  if (cabinet < 0 || cabinet >= 4 || !pace_on()) return;
  u8& queued = base[entry + queued_at];
  int& written = g_state.written[static_cast<std::size_t>(cabinet)];
  // A value this hook wrote and that no new frame has replaced since is left
  // as it is (not shortened twice).
  if (queued == written) return;
  queued = static_cast<u8>(queued > 0 ? queued - 1 : 0);
  written = queued;
}

void on_freeze() { ++g_state.freezes; }

}  // namespace link_pace
