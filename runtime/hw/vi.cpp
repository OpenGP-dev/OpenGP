// SPDX-License-Identifier: GPL-3.0-or-later

#include "hw/vi.h"

namespace hw {

namespace {

constexpr u32 VI_BEAM_V = 0x2C;
constexpr u32 VI_BEAM_H = 0x2E;
constexpr u32 VI_DI0 = 0x30;  // DI0..DI3: 32-bit each at 0x30, 0x34, 0x38, 0x3C
constexpr u32 VI_FB_LEFT_TOP_HI = 0x1C;

// Fields of the high half of a display interrupt register.
constexpr u16 DI_INT = 0x8000;   // interrupt status (bit 31)
constexpr u16 DI_MASK = 0x1000;  // interrupt enable (bit 28)
constexpr u16 DI_VCT = 0x03FF;   // vertical count (bits 16-25)

}  // namespace

ViDevice::ViDevice(Hardware& hw) : Device(hw) {
  origin_ = hw.timebase();
  // Reset values (Dolphin VideoInterfaceManager::Preset for NTSC): DI0 at
  // line 263 and DI1 at line 1, both enabled.
  regs_[VI_DI0 / 2] = DI_MASK | 263;
  regs_[VI_DI0 / 2 + 1] = 430;
  regs_[(VI_DI0 + 4) / 2] = DI_MASK | 1;
  regs_[(VI_DI0 + 4) / 2 + 1] = 1;
  for (int i = 0; i < 4; ++i) reschedule(i);
}

u32 ViDevice::xfb_address() const {
  const u32 top = (u32{regs_[VI_FB_LEFT_TOP_HI / 2]} << 16) | regs_[VI_FB_LEFT_TOP_HI / 2 + 1];
  // Bit 28 selects the address format: the address is either in units of 32
  // bytes (shifted by 5) or a plain physical address.
  const u32 addr = top & 0x00FFFFFF;
  return (top & 0x10000000) ? (addr << 5) : addr;
}

u16 ViDevice::read16(u32 offset) {
  const u64 t = hw_.timebase() - origin_;
  switch (offset) {
    case VI_BEAM_V: {
      const u32 half_line = static_cast<u32>((t / kTicksPerHalfLine) % kHalfLinesPerFrame);
      return static_cast<u16>(1 + half_line / 2);
    }
    case VI_BEAM_H: {
      const u64 in_line = t % (2 * kTicksPerHalfLine);
      return static_cast<u16>(1 + in_line * 858 / (2 * kTicksPerHalfLine));
    }
    default: return regs_[(offset / 2) % regs_.size()];
  }
}

void ViDevice::write16(u32 offset, u16 value) {
  offset %= regs_.size() * 2;
  if (offset == VI_BEAM_V || offset == VI_BEAM_H) return;  // read-only
  regs_[offset / 2] = value;
  if (offset >= VI_DI0 && offset < VI_DI0 + 16) {
    const int index = static_cast<int>((offset - VI_DI0) / 4);
    if (!(offset & 2)) {  // high half: status, enable, line
      reschedule(index);
      update_interrupts();
    }
  }
}

void ViDevice::reschedule(int index) {
  if (events_[index]) hw_.cancel(events_[index]);
  events_[index] = 0;
  const u16 hi = regs_[(VI_DI0 + 4 * index) / 2];
  const u32 vct = hi & DI_VCT;
  if (!(hi & DI_MASK) || vct == 0 || vct > kHalfLinesPerFrame / 2) return;
  const u64 target = u64{2 * (vct - 1)} * kTicksPerHalfLine;  // offset within a frame
  const u64 now = hw_.timebase() - origin_;
  u64 when = (now / kTicksPerFrame) * kTicksPerFrame + target;
  if (when <= now) when += kTicksPerFrame;
  events_[index] = hw_.schedule_at(origin_ + when, [this, index] {
    events_[index] = 0;
    regs_[(VI_DI0 + 4 * index) / 2] |= DI_INT;
    if (index == 0 || index == 1) ++retraces_;
    update_interrupts();
    reschedule(index);
  });
}

void ViDevice::update_interrupts() {
  bool active = false;
  for (int i = 0; i < 4; ++i) {
    const u16 hi = regs_[(VI_DI0 + 4 * i) / 2];
    if ((hi & DI_INT) && (hi & DI_MASK)) active = true;
  }
  hw_.set_interrupt(INT_VI, active);
}

u32 ViDevice::read(u32 offset, u32 size) {
  if (size == 4) return (u32{read16(offset)} << 16) | read16(offset + 2);
  if (size == 2) return read16(offset);
  return (read16(offset & ~1u) >> ((offset & 1) ? 0 : 8)) & 0xFF;
}

void ViDevice::write(u32 offset, u32 value, u32 size) {
  if (size == 4) {
    write16(offset, static_cast<u16>(value >> 16));
    write16(offset + 2, static_cast<u16>(value));
  } else if (size == 2) {
    write16(offset, static_cast<u16>(value));
  } else {
    u16 r = read16(offset & ~1u);
    r = (offset & 1) ? static_cast<u16>((r & 0xFF00) | (value & 0xFF)) : static_cast<u16>((r & 0xFF) | (value << 8));
    write16(offset & ~1u, r);
  }
}

}  // namespace hw
