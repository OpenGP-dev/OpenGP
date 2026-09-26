// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSP interface (0xCC005000): the CPU<->DSP mailboxes and control register,
// ARAM and its DMA, and the audio DMA to the AI.
//
// The DSP itself is emulated at a high level: the boot ROM and the
// __OSInitAudioSystem "init" microcode are reproduced; a game's own microcode
// is identified by hash when it is booted, and handled by a stub for now
// (audio is milestone 6). Based on Dolphin's DSP.cpp and DSPHLE
// (Copyright Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "hw/hardware.h"

namespace hw {

class DspDevice;

// A DSP program as seen from the CPU: it receives mail and may send mail back.
class DspUcode {
 public:
  explicit DspUcode(DspDevice& dsp) : dsp_(dsp) {}
  virtual ~DspUcode() = default;
  virtual void initialize() {}
  virtual void handle_mail(u32 mail) = 0;
  virtual const char* name() const = 0;

 protected:
  DspDevice& dsp_;
};

class DspDevice : public Device {
 public:
  explicit DspDevice(Hardware& hw);
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "DSP"; }

  // Called by microcode.
  void push_mail(u32 mail, bool interrupt = false);
  void boot_ucode(u32 ram_address, u32 length, u32 imem_address, u32 dmem_length, u32 start_pc);
  u8* aram() { return aram_.data(); }
  u32 aram_size() const { return static_cast<u32>(aram_.size()); }

  // DSPCR interrupt bits (status bit; the mask is the next bit up).
  enum : u16 { INT_AID = 0x08, INT_ARAM = 0x20, INT_DSP = 0x80 };
  void raise(u16 interrupt_bit);

 private:
  u16 read16(u32 offset);
  void write16(u32 offset, u16 value);
  u16 read_control();
  void write_control(u16 value);
  void set_ucode(std::unique_ptr<DspUcode> ucode);
  void do_aram_dma();
  void start_audio_dma();
  void audio_dma_block_done();
  void update_interrupts();

  // Control register: the DSP side owns reset/assert/halt/init bits.
  static constexpr u16 kDspSideMask = 0x0C07;
  u16 control_ = 0;       // CPU-side bits
  u16 dsp_control_ = 0;   // DSP-side bits
  u64 init_code_clear_time_ = 0;

  // Mailboxes
  u32 cpu_mailbox_ = 0;
  std::deque<std::pair<u32, bool>> mails_;  // DSP -> CPU, with "interrupt when read" flag
  u32 last_mail_ = 0;
  bool halted_ = true;
  std::unique_ptr<DspUcode> ucode_;

  // ARAM
  std::vector<u8> aram_;
  u16 ar_info_ = 0, ar_mode_ = 1, ar_refresh_ = 156;
  u32 ar_mmaddr_ = 0, ar_araddr_ = 0, ar_cnt_ = 0;

  // Audio DMA (to the AI)
  u32 audio_source_ = 0;
  u16 audio_control_ = 0;  // bit 15 enable, bits 0-14 length in 32-byte blocks
  u32 audio_current_ = 0;
  u32 audio_blocks_left_ = 0;
  u64 audio_block_start_ = 0;
  Hardware::EventId audio_event_ = 0;
};

}  // namespace hw
