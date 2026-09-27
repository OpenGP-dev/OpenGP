// SPDX-License-Identifier: GPL-3.0-or-later
//
// The graphics processor's front end: the command processor (CP, 0xCC000000)
// that reads the GX FIFO, and the pixel engine (PE, 0xCC001000) registers
// with the draw-done ("finish") and token interrupts that games wait on.
//
// FIFO data is decoded as soon as the CPU writes it (the FIFO is normally
// linked to the CPU's write-gather pipe). Nothing is rendered yet; register
// writes and draws go to the decoder's sink.
// Based on Dolphin's CommandProcessor.cpp and PixelEngine.cpp (Copyright
// Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "gx/decoder.h"
#include "hw/hardware.h"

namespace gx {

class Gpu : public Sink {
 public:
  explicit Gpu(hw::Hardware& hw);
  ~Gpu() override;

  // Register blocks to map at 0x0000 (CP) and 0x1000 (PE).
  std::unique_ptr<hw::Device> make_cp_device();
  std::unique_ptr<hw::Device> make_pe_device();

  // Called after each 32-byte burst of the write-gather pipe lands in RAM.
  void on_gather_burst();

  u64 frames() const { return copies_.load(std::memory_order_relaxed); }  // EFB copies
  u64 draws() const { return draws_.load(std::memory_order_relaxed); }

  // Sink
  void bp_write(u8 reg, u32 value) override;
  void draw(u8 primitive, u8 vat, u16 count, std::span<const u8> vertices) override;

 private:
  friend class CpDevice;
  friend class PeDevice;

  // CP
  u16 cp_reg_read(u32 offset);
  void cp_reg_write(u32 offset, u16 value);
  void run();  // consumes the FIFO from the read pointer
  void update_cp_interrupt();
  u32 distance() const;

  // PE
  u16 pe_reg_read(u32 offset);
  void pe_reg_write(u32 offset, u16 value);
  void update_pe_interrupts();
  void signal(bool finish, bool token_interrupt, u16 token);

  static std::span<const u8> read_display_list(void* context, u32 address, u32 size);

  hw::Hardware& hw_;
  Decoder decoder_;
  bool running_ = false;       // guards against re-entry while decoding
  std::vector<u8> pending_;    // FIFO bytes not yet forming a complete command

  // CP registers
  u16 cp_ctrl_ = 0x0010;  // linked, reads disabled
  u16 perf_select_ = 0, unk_0a_ = 0;
  u32 base_ = 0, end_ = 0, hi_watermark_ = 0x03FFFFE0, lo_watermark_ = 0;
  u32 rw_distance_ = 0, wptr_ = 0, rptr_ = 0, breakpoint_ = 0;
  bool cp_interrupt_ = false;

  // PE registers
  std::array<u16, 0x30 / 2> pe_regs_{};
  u16 pe_ctrl_ = 0;  // token/finish interrupt enables
  u16 token_ = 0;
  bool token_signaled_ = false, finish_signaled_ = false;

  std::atomic<u64> copies_{0}, draws_{0};
};

}  // namespace gx
