// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pausing the machine: the time base stops and carries on where it stopped,
// timed events wait, and guest code waits at its safe points. And the
// retraces on the host's clock, which even frame pacing shows frames at.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "hw/hardware.h"
#include "hw/vi.h"
#include "ppc/hooks.h"
#include "ppc/memory.h"

using namespace gekko;
using namespace std::chrono_literals;

namespace {

constexpr u64 ms_ticks(u64 ms) { return hw::kTimebaseHz / 1000 * ms; }

struct Rig {
  ppc::GuestMemory memory;
  std::unique_ptr<hw::Hardware> hw;

  Rig() {
    REQUIRE(memory.init(ppc::MEM1_SIZE_GAMECUBE));
    hw = std::make_unique<hw::Hardware>(memory.base(), memory.mem1_size());
    hw->start();
  }
  ~Rig() {
    hw->set_paused(false);
    hw->stop();
  }
};

}  // namespace

TEST_CASE("the time base stops while paused and carries on where it stopped") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  h.set_paused(true);
  CHECK(h.paused());
  const u64 at_pause = h.timebase();
  std::this_thread::sleep_for(50ms);
  CHECK(h.timebase() == at_pause);
  const auto before = std::chrono::steady_clock::now();
  h.set_paused(false);
  const u64 counted = h.timebase() - at_pause;
  const auto taken = std::chrono::steady_clock::now() - before;
  CHECK_FALSE(h.paused());
  // The 50 ms paused are not counted: only the time since unpausing is.
  CHECK(counted <= ms_ticks(1 + std::chrono::duration_cast<std::chrono::milliseconds>(taken).count()));
  std::this_thread::sleep_for(20ms);
  CHECK(h.timebase() - at_pause >= ms_ticks(20));
}

TEST_CASE("timed events wait while paused") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  std::atomic<bool> fired{false};
  // Paused first: an event due 20 ms later must wait for the unpause, however
  // slowly this thread runs.
  h.set_paused(true);
  h.schedule_after(ms_ticks(20), [&] { fired = true; });
  std::this_thread::sleep_for(80ms);
  CHECK_FALSE(fired.load());
  h.set_paused(false);
  for (int i = 0; i < 2000 && !fired; ++i) std::this_thread::sleep_for(1ms);
  CHECK(fired.load());
}

TEST_CASE("guest code stops at a safe point until resumed") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  CHECK_FALSE(ppc::g_interrupt_pending.load());
  h.set_paused(true);
  // A pause makes interrupts pending, so guest code reaches a safe point.
  CHECK(ppc::g_interrupt_pending.load());

  std::atomic<bool> through{false};
  std::thread guest([&] {
    h.wait_while_paused();
    through = true;
  });
  std::this_thread::sleep_for(30ms);
  CHECK_FALSE(through.load());
  h.set_paused(false);
  guest.join();
  CHECK(through.load());
  CHECK_FALSE(ppc::g_interrupt_pending.load());
}

TEST_CASE("retraces on the host's clock") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  h.map(0x2000, 0x100, std::make_unique<hw::ViDevice>(h));
  hw::ViDevice& vi = *h.device<hw::ViDevice>();
  const auto steady_ns = [] {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
  };
  // Field times: DI1 at line 1 starts a frame, DI0 at line 263 is 524
  // half-lines later, and the frame is 1050 half-lines.
  const u64 frame_ns = hw::ViDevice::kTicksPerFrame * 2000 / 81;
  const u64 field_ns = 524 * hw::ViDevice::kTicksPerHalfLine * 2000 / 81;
  const auto about = [](u64 a, u64 b) { return (a > b ? a - b : b - a) <= 1; };

  const u64 now = steady_ns();
  const u64 r1 = vi.next_retrace_ns(now);
  CHECK(r1 > now);
  CHECK(r1 - now <= frame_ns - field_ns + 1);
  const u64 r2 = vi.next_retrace_ns(r1);  // the next one, not the same
  CHECK((about(r2 - r1, field_ns) || about(r2 - r1, frame_ns - field_ns)));
  const u64 r3 = vi.next_retrace_ns(r2);
  CHECK(about(r3 - r1, frame_ns));
  CHECK(vi.next_retrace_ns(r1 - 1) == r1);

  vi.write(0x30, 0, 2);  // DI0 off: one retrace a frame
  CHECK(about(vi.next_retrace_ns(vi.next_retrace_ns(now)) - vi.next_retrace_ns(now), frame_ns));
  vi.write(0x34, 0, 2);  // DI1 off too: none
  CHECK(vi.next_retrace_ns(now) == 0);

  vi.write(0x34, 0x1001, 2);  // DI1 on at line 1 again
  h.set_paused(true);
  CHECK(vi.next_retrace_ns(steady_ns()) == 0);  // none while paused
  h.set_paused(false);
  CHECK(vi.next_retrace_ns(steady_ns()) != 0);
}

TEST_CASE("the time base can be set while paused") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  h.set_paused(true);
  h.set_timebase(12345);
  CHECK(h.timebase() == 12345);
  h.set_paused(false);
  CHECK(h.timebase() - 12345 < ms_ticks(25));
}
