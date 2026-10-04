// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSP interface (0xCC005000): the CPU<->DSP mailboxes and control register,
// ARAM and its DMA, and the audio DMA to the AI.
//
// The DSP itself is emulated at a high level: the boot ROM and the
// __OSInitAudioSystem "init" microcode are reproduced, and a game's own
// microcode is identified by hash when it is booted. AX (dsp/ax.h) is
// emulated; anything else gets a stub that only answers the task handshake.
// Based on Dolphin's DSP.cpp and DSPHLE (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "hw/hardware.h"

namespace hw {

class DspDevice;

// A DSP program as seen from the CPU: it receives mail and may send mail back.
class DspUcode {
 public:
  DspUcode(DspDevice& dsp, u32 crc) : dsp_(dsp), crc_(crc) {}
  virtual ~DspUcode() = default;
  virtual void initialize() {}
  virtual void handle_mail(u32 mail) = 0;
  virtual const char* name() const = 0;
  u32 crc() const { return crc_; }

  // Set when this microcode handed the DSP to another task: it sends
  // DSP_RESUME when it is booted again.
  bool needs_resume = false;

  // The SDK's DSP task manager protocol (Dolphin UCodes.h). The DSP reports
  // with 0xDCD1xxxx mails...
  static constexpr u32 TASK_MAIL_TO_CPU = 0xDCD10000;
  static constexpr u32 DSP_INIT = TASK_MAIL_TO_CPU | 0;
  static constexpr u32 DSP_RESUME = TASK_MAIL_TO_CPU | 1;
  static constexpr u32 DSP_YIELD = TASK_MAIL_TO_CPU | 2;
  static constexpr u32 DSP_DONE = TASK_MAIL_TO_CPU | 3;
  // ...and the CPU answers with 0xCDD1xxxx.
  static constexpr u32 TASK_MAIL_TO_DSP = 0xCDD10000;
  static constexpr u32 MAIL_RESUME = TASK_MAIL_TO_DSP | 0;
  static constexpr u32 MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 1;
  static constexpr u32 MAIL_RESET = TASK_MAIL_TO_DSP | 2;
  static constexpr u32 MAIL_CONTINUE = TASK_MAIL_TO_DSP | 3;

 protected:
  // After MAIL_NEW_UCODE the CPU describes the next task's microcode in ten
  // mails; the last one switches to it (Dolphin PrepareBootUCode).
  void prepare_boot_ucode(u32 mail);
  bool upload_in_progress_ = false;

  DspDevice& dsp_;

 private:
  u32 crc_;
  int boot_steps_ = 0;
  u32 boot_iram_addr_ = 0;
  u32 boot_iram_size_ = 0;
};

class DspDevice : public Device {
 public:
  explicit DspDevice(Hardware& hw);
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "DSP"; }

  // Receives the audio DMA's samples as each buffer starts playing:
  // interleaved stereo (left, right) at `rate` Hz. Runs on the timer thread
  // with the hardware mutex held, so it must be quick.
  using AudioSink = std::function<void(const s16* samples, u32 frames, u32 rate)>;
  void set_audio_sink(AudioSink sink) { audio_sink_ = std::move(sink); }

  // Called by microcode.
  // Queues a mail for the CPU; with `interrupt`, the DSP interrupt is raised
  // (after `delay` time base ticks) once the CPU has read the mails before it.
  void push_mail(u32 mail, bool interrupt = false, u64 delay = 0);
  void boot_ucode(u32 ram_address, u32 length, u32 imem_address, u32 dmem_length, u32 start_pc);
  // Switches to the microcode with hash `crc`, or back to the one that
  // switched away (Dolphin DSPHLE::SwapUCode).
  void swap_ucode(u32 crc);
  void reset_ucode();  // back to the boot ROM
  u8* ram(u32 addr, u32 size) const { return hw_.ram(addr, size); }
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
  std::unique_ptr<DspUcode> make_ucode(u32 crc);
  void do_aram_dma();
  void start_audio_dma();
  void latch_audio_dma(u64 start);
  void audio_dma_done();
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
  std::unique_ptr<DspUcode> last_ucode_;  // the task that switched away, to switch back to
  std::unique_ptr<DspUcode> retired_ucode_;  // replaced while one of its methods was running

  // ARAM
  std::vector<u8> aram_;
  u16 ar_info_ = 0, ar_mode_ = 1, ar_refresh_ = 156;
  u32 ar_mmaddr_ = 0, ar_araddr_ = 0, ar_cnt_ = 0;

  // Audio DMA (to the AI)
  u32 audio_source_ = 0;
  u16 audio_control_ = 0;  // bit 15 enable, bits 0-14 length in 32-byte blocks
  u32 audio_blocks_left_ = 0;  // in the playing buffer, at audio_block_start_
  u64 audio_block_start_ = 0;
  u64 audio_block_ticks_ = 0;
  u64 audio_buffer_end_ = 0;
  Hardware::EventId audio_event_ = 0;
  AudioSink audio_sink_;
  std::vector<s16> audio_samples_;
};

}  // namespace hw
