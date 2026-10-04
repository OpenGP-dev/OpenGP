// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DSP's ARAM accelerator: the hardware that reads a voice's samples from
// ARAM, decoding 4-bit ADPCM (or scaling 8/16-bit PCM) as it goes, and raises
// an exception when it reaches the voice's end address so the microcode can
// loop or stop the voice.
//
// Ported from Dolphin's Core/DSP/DSPAccelerator.cpp (Copyright 2008 Dolphin
// Emulator Project, GPL-2.0-or-later). Only sample reads are needed here; the
// raw reads and writes the Zelda microcode uses are left out.
#pragma once

#include "gekko/types.h"

namespace dsp {

using namespace gekko;

class Accelerator {
 public:
  virtual ~Accelerator() = default;

  // Reads and decodes the next sample. `coefs` are the voice's 8 ADPCM
  // predictor coefficient pairs.
  u16 read_sample(const s16* coefs);

  u32 current_address() const { return current_address_; }
  s16 yn1() const { return yn1_; }
  s16 yn2() const { return yn2_; }
  u16 pred_scale() const { return pred_scale_; }

  void set_start_address(u32 address) { start_address_ = address & 0x3FFFFFFF; }
  void set_end_address(u32 address) { end_address_ = address & 0x3FFFFFFF; }
  void set_current_address(u32 address) { current_address_ = address & 0xBFFFFFFF; }
  void set_sample_format(u16 format) { format_ = format; }
  void set_gain(s16 gain) { gain_ = gain; }
  void set_yn1(s16 yn1) { yn1_ = yn1; }
  // Writing YN2 also resumes reads stopped at the end address.
  void set_yn2(s16 yn2) {
    yn2_ = yn2;
    reads_stopped_ = false;
  }
  void set_pred_scale(u16 pred_scale) { pred_scale_ = pred_scale & 0x7F; }

 protected:
  // The end address was reached: the microcode reloads the loop state (or
  // stops the voice).
  virtual void on_sample_read_end() = 0;
  virtual u8 read_memory(u32 address) = 0;

 private:
  u16 current_sample();

  u32 start_address_ = 0;
  u32 end_address_ = 0;
  u32 current_address_ = 0;
  // Sample format: bits 0-1 access size (4/8/16-bit), bits 2-3 decoding
  // (ADPCM, PCM from ACIN, PCM, PCM from ACIN with increment), bits 4-5 PCM
  // gain scale.
  u16 format_ = 0;
  s16 gain_ = 0;
  s16 yn1_ = 0;
  s16 yn2_ = 0;
  u16 pred_scale_ = 0;
  // After the end-address exception the accelerator returns zeros and stops
  // moving until YN2 is written.
  bool reads_stopped_ = false;
};

}  // namespace dsp
