// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio interface (0xCC006C00): the streaming sample counter and its
// interrupt, and the sample rates of streaming (AIS) and DMA (AID) audio.
// No sound is produced here; the counter runs in real time so that the SDK's
// timing calibration and audio code see a working clock.
// Based on Dolphin's AudioInterface.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include "hw/hardware.h"

namespace hw {

class AiDevice : public Device {
 public:
  explicit AiDevice(Hardware& hw) : Device(hw) {}
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "AI"; }

  // Sample rate of DMA audio (from the DSP), in Hz.
  u32 dma_sample_rate() const;

 private:
  bool playing() const { return control_ & 1; }
  u32 samples_since(u64 tb) const;  // streamed samples since time base `tb`
  u64 ticks_for(u32 samples) const;
  u32 sample_counter() const;
  void reschedule();
  void update_interrupts();

  u32 control_ = 0;
  u32 volume_ = 0;
  u32 counter_ = 0;   // sample count at time `counter_time_`
  u64 counter_time_ = 0;
  u32 interrupt_timing_ = 0;
  Hardware::EventId event_ = 0;
};

}  // namespace hw
