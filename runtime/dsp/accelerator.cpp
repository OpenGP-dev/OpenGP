// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Dolphin's Core/DSP/DSPAccelerator.cpp (Copyright 2008 Dolphin
// Emulator Project, GPL-2.0-or-later).

#include "dsp/accelerator.h"

#include <algorithm>

namespace dsp {

namespace {

enum : u16 { SIZE_4BIT = 0, SIZE_8BIT = 1, SIZE_16BIT = 2 };
enum : u16 { DECODE_ADPCM = 0, DECODE_MMIO_PCM_NO_INC = 1, DECODE_PCM = 2, DECODE_MMIO_PCM_INC = 3 };

s16 saturate16(s32 v) { return static_cast<s16>(std::clamp(v, -0x8000, 0x7FFF)); }

}  // namespace

u16 Accelerator::current_sample() {
  switch (format_ & 3) {
    case SIZE_4BIT: {
      const u16 byte = read_memory(current_address_ >> 1);
      return (current_address_ & 1) ? (byte & 0xF) : (byte >> 4);
    }
    case SIZE_8BIT: return read_memory(current_address_);
    case SIZE_16BIT:
      return static_cast<u16>((read_memory(current_address_ * 2) << 8) | read_memory(current_address_ * 2 + 1));
    default: return 0;  // garbage on hardware
  }
}

u16 Accelerator::read_sample(const s16* coefs) {
  if (reads_stopped_) return 0;

  const u16 decode = (format_ >> 2) & 3;
  // The MMIO modes read ACIN, which nothing here writes.
  s16 raw = (decode == DECODE_MMIO_PCM_NO_INC || decode == DECODE_MMIO_PCM_INC) ? 0
                                                                                 : static_cast<s16>(current_sample());
  const int coef_idx = (pred_scale_ >> 4) & 7;
  const s32 coef1 = coefs[coef_idx * 2 + 0];
  const s32 coef2 = coefs[coef_idx * 2 + 1];

  u16 val = 0;
  u8 step_size = 0;
  if (decode == DECODE_ADPCM) {
    // Only 4-bit decoding exists; wider reads keep their low nibble.
    raw &= 0xF;
    if (raw >= 8) raw -= 16;
    const int scale = 1 << (pred_scale_ & 0xF);
    val = static_cast<u16>(saturate16(scale * raw + ((0x400 + coef1 * yn1_ + coef2 * yn2_) >> 11)));
    step_size = 2;
    yn2_ = yn1_;
    yn1_ = static_cast<s16>(val);
    current_address_ += 1;

    // An end address at the start of a frame (or one nibble in) loops without
    // the exception and without reloading the predictor/scale.
    if ((end_address_ & 0xF) == 0x0 && current_address_ == end_address_) {
      current_address_ = start_address_ + 1;
    } else if ((end_address_ & 0xF) == 0x1 && current_address_ == end_address_ - 1) {
      current_address_ = start_address_;
    } else if ((current_address_ & 15) == 0) {
      // Each 8-byte frame starts with its predictor/scale byte.
      pred_scale_ = read_memory((current_address_ & ~15u) >> 1);
      current_address_ += 2;
      step_size += 2;
    }
  } else {
    // PCM: the gain applies, then the same prediction as ADPCM.
    int gain_shift = 0;
    switch ((format_ >> 4) & 3) {
      case 0: gain_shift = 11; break;
      case 1: gain_shift = 0; break;
      case 2: gain_shift = 16; break;
      default: break;
    }
    const s32 val32 = ((static_cast<s32>(gain_) * raw) >> gain_shift) +
                      (((coef1 * yn1_) >> gain_shift) + ((coef2 * yn2_) >> gain_shift));
    val = static_cast<u16>(val32);
    yn2_ = yn1_;
    yn1_ = static_cast<s16>(val);
    step_size = 2;
    if (decode != DECODE_MMIO_PCM_NO_INC) current_address_ += 1;
  }

  // The end of the sample: back to the start, and the microcode reloads the
  // loop state (YN1/YN2) from the voice.
  if (current_address_ == end_address_ + step_size - 1) {
    current_address_ = start_address_;
    reads_stopped_ = true;
    on_sample_read_end();
  }

  set_current_address(current_address_);
  return val;
}

}  // namespace dsp
