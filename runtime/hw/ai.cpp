// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's AudioInterface.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).

#include "hw/ai.h"

namespace hw {

namespace {

enum : u32 { AI_CONTROL = 0x00, AI_VOLUME = 0x04, AI_SAMPLE_COUNTER = 0x08, AI_INTERRUPT_TIMING = 0x0C };

// Control register.
constexpr u32 CR_PSTAT = 1u << 0;     // streaming sample counter running
constexpr u32 CR_AISFR = 1u << 1;     // streaming rate: 1 = 48 kHz, 0 = 32 kHz
constexpr u32 CR_AIINTMSK = 1u << 2;
constexpr u32 CR_AIINT = 1u << 3;
constexpr u32 CR_AIINTVLD = 1u << 4;
constexpr u32 CR_SCRESET = 1u << 5;   // write: reset the sample counter
constexpr u32 CR_AIDFR = 1u << 6;     // DMA rate: 1 = 32 kHz, 0 = 48 kHz

// Rates are 108 MHz / divisor (48043 Hz and 32028 Hz on the GameCube).
constexpr u64 kRateDividend = 108000000;
constexpr u64 kDivisor48k = 1124 * 2;
constexpr u64 kDivisor32k = kDivisor48k * 3 / 2;
// Time base ticks per 108 MHz tick: 3/8.
static_assert(kTimebaseHz * 8 == kRateDividend * 3);

}  // namespace

u32 AiDevice::dma_sample_rate() const {
  return static_cast<u32>(kRateDividend / ((control_ & CR_AIDFR) ? kDivisor32k : kDivisor48k));
}

u32 AiDevice::samples_since(u64 tb) const {
  const u64 divisor = (control_ & CR_AISFR) ? kDivisor48k : kDivisor32k;
  const u64 now = hw_.timebase();
  if (now <= tb) return 0;
  return static_cast<u32>((now - tb) * 8 / (3 * divisor));
}

u64 AiDevice::ticks_for(u32 samples) const {
  const u64 divisor = (control_ & CR_AISFR) ? kDivisor48k : kDivisor32k;
  return (u64{samples} * divisor * 3 + 7) / 8;
}

u32 AiDevice::sample_counter() const { return counter_ + (playing() ? samples_since(counter_time_) : 0); }

u32 AiDevice::read(u32 offset, u32 size) {
  if (size != 4) return 0;
  switch (offset) {
    case AI_CONTROL: return control_;
    case AI_VOLUME: return volume_;
    case AI_SAMPLE_COUNTER: return sample_counter();
    case AI_INTERRUPT_TIMING: return interrupt_timing_;
    default: return 0;
  }
}

void AiDevice::write(u32 offset, u32 v, u32 size) {
  if (size != 4) return;
  switch (offset) {
    case AI_CONTROL: {
      // Keep the counter continuous across rate and run-state changes.
      counter_ = sample_counter();
      counter_time_ = hw_.timebase();
      constexpr u32 kWritable = CR_PSTAT | CR_AISFR | CR_AIINTMSK | CR_AIINTVLD | CR_AIDFR;
      control_ = (control_ & ~kWritable) | (v & kWritable);
      if (v & CR_AIINT) control_ &= ~CR_AIINT;
      if (v & CR_SCRESET) counter_ = 0;
      reschedule();
      update_interrupts();
      break;
    }
    case AI_VOLUME: volume_ = v; break;
    case AI_SAMPLE_COUNTER:
      counter_ = v;
      counter_time_ = hw_.timebase();
      reschedule();
      break;
    case AI_INTERRUPT_TIMING:
      interrupt_timing_ = v;
      reschedule();
      break;
    default: break;
  }
}

void AiDevice::reschedule() {
  if (event_) hw_.cancel(event_), event_ = 0;
  if (!playing()) return;
  // Interrupt when the counter reaches the interrupt timing value.
  u32 remaining = interrupt_timing_ - sample_counter();
  if (remaining == 0 || remaining > 0x80000000u) remaining = 0x80000000u;  // passed: far away
  event_ = hw_.schedule_after(ticks_for(remaining), [this] {
    event_ = 0;
    if (!playing()) return;
    if (sample_counter() - interrupt_timing_ < 0x80000000u) {
      control_ |= CR_AIINT;
      update_interrupts();
    }
    reschedule();
  });
}

void AiDevice::update_interrupts() {
  hw_.set_interrupt(INT_AI, (control_ & CR_AIINT) && (control_ & CR_AIINTMSK));
}

}  // namespace hw
