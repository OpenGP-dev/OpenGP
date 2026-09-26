// SPDX-License-Identifier: GPL-3.0-or-later

#include "hw/dsp.h"

#include <cstdio>
#include <cstring>

namespace hw {

namespace {

// DSP interface registers (offsets from 0xCC005000).
enum : u32 {
  MAIL_TO_DSP_HI = 0x00,
  MAIL_TO_DSP_LO = 0x02,
  MAIL_FROM_DSP_HI = 0x04,
  MAIL_FROM_DSP_LO = 0x06,
  CONTROL = 0x0A,
  AR_INFO = 0x12,
  AR_MODE = 0x16,
  AR_REFRESH = 0x1A,
  AR_DMA_MMADDR_H = 0x20,
  AR_DMA_MMADDR_L = 0x22,
  AR_DMA_ARADDR_H = 0x24,
  AR_DMA_ARADDR_L = 0x26,
  AR_DMA_CNT_H = 0x28,
  AR_DMA_CNT_L = 0x2A,
  AUDIO_DMA_START_HI = 0x30,
  AUDIO_DMA_START_LO = 0x32,
  AUDIO_DMA_CONTROL_LEN = 0x36,
  AUDIO_DMA_BLOCKS_LEFT = 0x3A,
};

// DSPCR bits
constexpr u16 CR_RESET = 0x0001;
constexpr u16 CR_HALT = 0x0004;
constexpr u16 CR_DMA_STATE = 0x0200;
constexpr u16 CR_INIT_CODE = 0x0400;
constexpr u16 CR_INIT = 0x0800;
constexpr u16 CR_INT_BITS = DspDevice::INT_AID | DspDevice::INT_ARAM | DspDevice::INT_DSP;
constexpr u16 CR_MASK_BITS = CR_INT_BITS << 1;

constexpr u32 kAramSize = 16 * 1024 * 1024;
constexpr u64 kAudioBlockTicks = kTimebaseHz / 4000;  // one 32-byte block at 32 kHz stereo

// Dolphin's Common::HashEctor, which its DSP HLE uses to identify microcode.
u32 hash_ector(const u8* data, u32 len) {
  u32 crc = 0;
  for (u32 i = 0; i < len; ++i) {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}

// A game's own microcode, not emulated yet. It announces itself the way the
// SDK's DSP task manager expects (DSP_INIT) and logs the mail it receives.
class TaskUcode : public DspUcode {
 public:
  TaskUcode(DspDevice& dsp, u32 crc) : DspUcode(dsp), crc_(crc) {}
  void initialize() override { dsp_.push_mail(0xDCD10000, true); }
  void handle_mail(u32 mail) override {
    if (logged_ < 32) {
      std::fprintf(stderr, "[dsp] ucode %08X: mail %08X (not emulated)\n", crc_, mail);
      ++logged_;
    }
  }
  const char* name() const override { return "task"; }

 private:
  u32 crc_;
  int logged_ = 0;
};

// The microcode __OSInitAudioSystem runs: it only says hello.
class InitUcode : public DspUcode {
 public:
  using DspUcode::DspUcode;
  void initialize() override { dsp_.push_mail(0x80544348); }
  void handle_mail(u32) override {}
  const char* name() const override { return "init"; }
};

// The DSP boot ROM: greets, then accepts a microcode upload sequence.
class RomUcode : public DspUcode {
 public:
  using DspUcode::DspUcode;
  void initialize() override { dsp_.push_mail(0x8071FEED); }
  const char* name() const override { return "rom"; }
  void handle_mail(u32 mail) override {
    if (next_ == 0) {
      if ((mail & 0xFFFF0000) != 0x80F30000) dsp_.push_mail(0xFEEE0000 | (mail & 0xFFFF));
      else next_ = mail;
      return;
    }
    switch (next_) {
      case 0x80F3A001: ram_address_ = mail; break;
      case 0x80F3A002: length_ = mail & 0xFFFF; break;
      case 0x80F3B002: dmem_length_ = mail & 0xFFFF; break;
      case 0x80F3C002: imem_address_ = mail & 0xFFFF; break;
      case 0x80F3D001:
        dsp_.boot_ucode(ram_address_, length_, imem_address_, dmem_length_, mail & 0xFFFF);
        return;  // this object has been replaced
      default: break;
    }
    next_ = 0;
  }

 private:
  u32 next_ = 0;
  u32 ram_address_ = 0, length_ = 0, dmem_length_ = 0, imem_address_ = 0;
};

}  // namespace

DspDevice::DspDevice(Hardware& hw) : Device(hw), aram_(kAramSize, 0) {
  dsp_control_ = CR_HALT | CR_INIT;
  set_ucode(std::make_unique<RomUcode>(*this));
}

void DspDevice::set_ucode(std::unique_ptr<DspUcode> ucode) {
  mails_.clear();
  ucode_ = std::move(ucode);
  ucode_->initialize();
}

void DspDevice::boot_ucode(u32 ram_address, u32 length, u32 imem_address, u32 dmem_length, u32 start_pc) {
  const u8* code = hw_.ram(ram_address, length);
  const u32 crc = code ? hash_ector(code, length) : 0;
  std::fprintf(stderr, "[dsp] booting ucode: crc %08X, %u bytes from %08X (imem %04X, dmem %u, pc %04X)\n", crc,
               length, ram_address, imem_address, dmem_length, start_pc);
  set_ucode(std::make_unique<TaskUcode>(*this, crc));
}

void DspDevice::push_mail(u32 mail, bool interrupt) {
  if (interrupt) {
    if (mails_.empty()) raise(INT_DSP);
    else mails_.front().second = true;
  }
  mails_.emplace_back(mail, false);
}

void DspDevice::raise(u16 interrupt_bit) {
  control_ |= interrupt_bit & CR_INT_BITS;
  update_interrupts();
}

void DspDevice::update_interrupts() {
  const bool active = ((control_ >> 1) & control_ & CR_INT_BITS) != 0;
  hw_.set_interrupt(INT_DSP, active);
}

u16 DspDevice::read_control() {
  if ((dsp_control_ & CR_INIT_CODE) && hw_.timebase() >= init_code_clear_time_) dsp_control_ &= ~CR_INIT_CODE;
  return static_cast<u16>((control_ & ~kDspSideMask) | (dsp_control_ & kDspSideMask));
}

void DspDevice::write_control(u16 value) {
  // DSP side (Dolphin DSPHLE::DSP_WriteControlRegister).
  u16 dsp = value & kDspSideMask;
  halted_ = (dsp & CR_HALT) != 0;
  if (dsp & CR_RESET) {
    set_ucode(std::make_unique<RomUcode>(*this));
    dsp &= ~CR_RESET;
    audio_control_ = 0;
  }
  if ((dsp_control_ & CR_INIT) && !(dsp & CR_INIT)) {
    // Clearing DSPInit runs the init microcode copied to 0x81000000.
    set_ucode(std::make_unique<InitUcode>(*this));
    dsp |= CR_INIT_CODE;
    init_code_clear_time_ = hw_.timebase() + 130;
  }
  dsp_control_ = dsp;

  // CPU side: masks and DMA state are written; interrupt bits are cleared by writing 1.
  control_ = static_cast<u16>((control_ & (CR_INT_BITS | CR_DMA_STATE)) | (value & CR_MASK_BITS));
  control_ &= static_cast<u16>(~(value & CR_INT_BITS));
  update_interrupts();
}

u16 DspDevice::read16(u32 offset) {
  switch (offset) {
    case MAIL_TO_DSP_HI: return static_cast<u16>(cpu_mailbox_ >> 16);
    case MAIL_TO_DSP_LO: return static_cast<u16>(cpu_mailbox_);
    case MAIL_FROM_DSP_HI:
      if (!halted_ && !mails_.empty()) last_mail_ = mails_.front().first;
      return static_cast<u16>(last_mail_ >> 16);
    case MAIL_FROM_DSP_LO: {
      if (!halted_ && !mails_.empty()) {
        last_mail_ = mails_.front().first;
        const bool interrupt = mails_.front().second;
        mails_.pop_front();
        if (interrupt) raise(INT_DSP);
      }
      last_mail_ &= ~0x80000000u;  // the next high read shows no mail
      return static_cast<u16>(last_mail_);
    }
    case CONTROL: return read_control();
    case AR_INFO: return ar_info_;
    case AR_MODE: return ar_mode_;
    case AR_REFRESH: return ar_refresh_;
    case AR_DMA_MMADDR_H: return static_cast<u16>(ar_mmaddr_ >> 16);
    case AR_DMA_MMADDR_L: return static_cast<u16>(ar_mmaddr_);
    case AR_DMA_ARADDR_H: return static_cast<u16>(ar_araddr_ >> 16);
    case AR_DMA_ARADDR_L: return static_cast<u16>(ar_araddr_);
    case AR_DMA_CNT_H: return static_cast<u16>(ar_cnt_ >> 16);
    case AR_DMA_CNT_L: return static_cast<u16>(ar_cnt_);
    case AUDIO_DMA_START_HI: return static_cast<u16>(audio_source_ >> 16);
    case AUDIO_DMA_START_LO: return static_cast<u16>(audio_source_);
    case AUDIO_DMA_CONTROL_LEN: return audio_control_;
    case AUDIO_DMA_BLOCKS_LEFT: {
      // Zero-based count of blocks left in the current buffer.
      if (!(audio_control_ & 0x8000) || audio_blocks_left_ == 0) return 0;
      const u64 elapsed = (hw_.timebase() - audio_block_start_) / kAudioBlockTicks;
      const u64 left = elapsed >= audio_blocks_left_ ? 0 : audio_blocks_left_ - elapsed;
      return static_cast<u16>(left > 0 ? left - 1 : 0);
    }
    default: return 0;
  }
}

void DspDevice::write16(u32 offset, u16 v) {
  switch (offset) {
    case MAIL_TO_DSP_HI: cpu_mailbox_ = (cpu_mailbox_ & 0xFFFF) | (u32{v} << 16); break;
    case MAIL_TO_DSP_LO:
      cpu_mailbox_ = (cpu_mailbox_ & 0xFFFF0000) | v;
      if (ucode_) ucode_->handle_mail(cpu_mailbox_);
      cpu_mailbox_ &= 0x7FFFFFFF;  // processed
      break;
    case CONTROL: write_control(v); break;
    case AR_INFO: ar_info_ = v & 0x7F; break;
    case AR_REFRESH: ar_refresh_ = v & 0x7FF; break;
    case AR_DMA_MMADDR_H: ar_mmaddr_ = (ar_mmaddr_ & 0xFFFF) | (u32{v & 0x3FFu} << 16); break;
    case AR_DMA_MMADDR_L: ar_mmaddr_ = (ar_mmaddr_ & 0xFFFF0000) | (v & 0xFFE0u); break;
    case AR_DMA_ARADDR_H: ar_araddr_ = (ar_araddr_ & 0xFFFF) | (u32{v & 0x3FFu} << 16); break;
    case AR_DMA_ARADDR_L: ar_araddr_ = (ar_araddr_ & 0xFFFF0000) | (v & 0xFFE0u); break;
    case AR_DMA_CNT_H: ar_cnt_ = (ar_cnt_ & 0xFFFF) | (u32{v & 0x83FFu} << 16); break;
    case AR_DMA_CNT_L:
      ar_cnt_ = (ar_cnt_ & 0xFFFF0000) | (v & 0xFFE0u);
      do_aram_dma();
      break;
    case AUDIO_DMA_START_HI: audio_source_ = (audio_source_ & 0xFFFF) | (u32{v & 0x3FFu} << 16); break;
    case AUDIO_DMA_START_LO: audio_source_ = (audio_source_ & 0xFFFF0000) | (v & 0xFFE0u); break;
    case AUDIO_DMA_CONTROL_LEN: {
      const bool was_enabled = audio_control_ & 0x8000;
      audio_control_ = v;
      if (!was_enabled && (v & 0x8000)) start_audio_dma();
      break;
    }
    default: break;
  }
}

u32 DspDevice::read(u32 offset, u32 size) {
  if (size == 4) return (u32{read16(offset)} << 16) | read16(offset + 2);
  return read16(offset & ~1u);
}

void DspDevice::write(u32 offset, u32 value, u32 size) {
  if (size == 4) {
    write16(offset, static_cast<u16>(value >> 16));
    write16(offset + 2, static_cast<u16>(value));
  } else {
    write16(offset & ~1u, static_cast<u16>(value));
  }
}

// ARAM DMA completes instantly; its interrupt is delivered at the next safe point.
void DspDevice::do_aram_dma() {
  const bool to_mram = ar_cnt_ & 0x80000000;
  u32 count = ar_cnt_ & 0x03FFFFE0;
  u32 mm = ar_mmaddr_ & 0x03FFFFFF;
  u32 ar = ar_araddr_ & 0x03FFFFFF;
  const u32 mask = kAramSize - 1;
  while (count) {
    u8* ram = hw_.ram(mm, 32);
    if (ram) {
      for (u32 i = 0; i < 32; ++i) {
        if (to_mram) ram[i] = aram_[(ar + i) & mask];
        else aram_[(ar + i) & mask] = ram[i];
      }
    }
    mm += 32;
    ar += 32;
    count -= 32;
  }
  ar_mmaddr_ = mm;
  ar_araddr_ = ar;
  ar_cnt_ &= 0x80000000;
  raise(INT_ARAM);
}

void DspDevice::start_audio_dma() {
  audio_current_ = audio_source_;
  audio_blocks_left_ = audio_control_ & 0x7FFF;
  audio_block_start_ = hw_.timebase();
  if (audio_event_) hw_.cancel(audio_event_);
  // The interrupt fires as a buffer starts, so the game can queue the next one.
  hw_.schedule_after(8, [this] { raise(INT_AID); });
  audio_event_ = hw_.schedule_after(kAudioBlockTicks * (audio_blocks_left_ ? audio_blocks_left_ : 1),
                                    [this] { audio_dma_block_done(); });
}

void DspDevice::audio_dma_block_done() {
  audio_event_ = 0;
  if (!(audio_control_ & 0x8000)) return;
  // The buffer finished: reload from the (possibly updated) registers.
  audio_current_ = audio_source_;
  audio_blocks_left_ = audio_control_ & 0x7FFF;
  audio_block_start_ = hw_.timebase();
  raise(INT_AID);
  audio_event_ = hw_.schedule_after(kAudioBlockTicks * (audio_blocks_left_ ? audio_blocks_left_ : 1),
                                    [this] { audio_dma_block_done(); });
}

}  // namespace hw
