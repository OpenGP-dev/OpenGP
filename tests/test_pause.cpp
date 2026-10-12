// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pausing the machine: the time base stops and carries on where it stopped,
// timed events wait, and guest code waits at its safe points.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "hw/hardware.h"
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
  h.set_paused(false);
  CHECK_FALSE(h.paused());
  // The 50 ms paused are not counted (allow for a slow scheduler).
  CHECK(h.timebase() - at_pause < ms_ticks(25));
  std::this_thread::sleep_for(20ms);
  CHECK(h.timebase() - at_pause >= ms_ticks(20));
}

TEST_CASE("timed events wait while paused") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  std::atomic<bool> fired{false};
  h.schedule_after(ms_ticks(20), [&] { fired = true; });
  h.set_paused(true);
  std::this_thread::sleep_for(80ms);
  CHECK_FALSE(fired.load());
  h.set_paused(false);
  for (int i = 0; i < 200 && !fired; ++i) std::this_thread::sleep_for(1ms);
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

TEST_CASE("the time base can be set while paused") {
  Rig rig;
  hw::Hardware& h = *rig.hw;
  h.set_paused(true);
  h.set_timebase(12345);
  CHECK(h.timebase() == 12345);
  h.set_paused(false);
  CHECK(h.timebase() - 12345 < ms_ticks(25));
}
