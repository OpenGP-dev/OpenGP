// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video interface (0xCC002000): display timing, the beam position, the four
// display interrupts (DI0-DI3, which the SDK uses for retrace), and the
// external framebuffer (XFB) addresses the display scans out.
//
// Timing is fixed NTSC 480i (the Triforce always runs NTSC): 525 lines per
// frame at 30000/1001 Hz, i.e. 1050 half-lines of 1287 time-base ticks each.
// Based on Dolphin's VideoInterface.cpp (GPL-2.0-or-later).
#pragma once

#include <array>

#include "hw/hardware.h"

namespace hw {

class ViDevice : public Device {
 public:
  explicit ViDevice(Hardware& hw);
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "VI"; }

  // Physical address of the XFB (top field) the display shows.
  u32 xfb_address() const;
  u64 retrace_count() const { return retraces_; }
  // The host's steady clock (in nanoseconds) at the first retrace after
  // `after_ns`: DI0's or DI1's interrupt, both of which the SDK counts as a
  // retrace (one a field, 59.94 a second). The display shows the XFB the game
  // gave it from then. 0 if neither interrupt is on, or while the machine is
  // paused. Thread-safe.
  u64 next_retrace_ns(u64 after_ns);

  static constexpr u64 kTicksPerHalfLine = 1287;
  static constexpr u32 kHalfLinesPerFrame = 1050;
  static constexpr u64 kTicksPerFrame = kTicksPerHalfLine * kHalfLinesPerFrame;  // 1351350

 private:
  u16 read16(u32 offset);
  void write16(u32 offset, u16 value);
  // The time base of display interrupt `index`'s first time after `timebase`,
  // or 0 if it is off.
  u64 next_interrupt(int index, u64 timebase) const;
  void reschedule(int index);
  void update_interrupts();

  std::array<u16, 0x80> regs_{};
  u64 origin_ = 0;  // time base at the start of frame 0
  std::array<Hardware::EventId, 4> events_{};
  u64 retraces_ = 0;
};

}  // namespace hw
