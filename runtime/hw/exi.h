// SPDX-License-Identifier: GPL-3.0-or-later
//
// Expansion interface (0xCC006800): three serial channels, each with up to
// three chip-selected devices. On the Triforce:
//   channel 0, CS 1: memory card slot A (empty)
//   channel 0, CS 2: IPL chip (boot ROM, RTC, SRAM, UART)
//   channel 0, CS 4: AM-Baseboard (serial port 1): backup memory
//   channel 1, CS 1: memory card slot B (empty)
// Based on Dolphin's EXI_Channel.cpp, EXI_DeviceIPL.cpp and
// EXI_DeviceBaseboard.cpp (Copyright Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <cstdio>
#include <memory>
#include <string>

#include "hw/hardware.h"

namespace hw {

class ExiDevice {
 public:
  virtual ~ExiDevice() = default;
  virtual bool present() const { return true; }
  virtual void set_cs(bool) {}
  virtual void transfer_byte(u8& byte) = 0;
  virtual bool interrupt() { return false; }
  // Immediate transfers move up to 4 bytes, most significant first.
  virtual void imm_write(u32 data, u32 size);
  virtual u32 imm_read(u32 size);
  virtual void dma_write(u8* src, u32 size);  // memory -> device
  virtual void dma_read(u8* dst, u32 size);   // device -> memory
};

// The IPL chip: RTC, SRAM (64 bytes of settings) and the debug UART.
class IplDevice : public ExiDevice {
 public:
  IplDevice();
  void set_cs(bool selected) override;
  void transfer_byte(u8& byte) override;

 private:
  std::array<u8, 0x44> sram_{};  // RTC (4 bytes) + 64 bytes of settings
  u32 command_ = 0;
  u32 command_bytes_ = 0;
  u32 cursor_ = 0;
  std::string uart_line_;
};

// The Triforce AM-Baseboard's EXI side: a backup memory store (kept in a file).
class BaseboardDevice : public ExiDevice {
 public:
  explicit BaseboardDevice(std::string backup_path);
  ~BaseboardDevice() override;
  void set_cs(bool selected) override;
  void transfer_byte(u8& byte) override;
  bool interrupt() override;
  void dma_write(u8* src, u32 size) override;
  void dma_read(u8* dst, u32 size) override;
  // Raised by the media board when a command completes (`flag`: which one).
  void generate_interrupt(u32 flag) {
    interrupt_set_ = true;
    irq_timer_ = 0;
    irq_status_ = flag;
  }

 private:
  std::FILE* backup_ = nullptr;
  std::array<u8, 4> command_{};
  u32 position_ = 0;
  u32 backup_offset_ = 0;
  bool interrupt_set_ = false;
  u32 irq_timer_ = 0;
  u32 irq_status_ = 0;
};

class ExiBus : public Device {
 public:
  ExiBus(Hardware& hw, const std::string& user_dir, const std::string& game_id);
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "EXI"; }
  // Raises the baseboard's interrupt (media board command completion).
  void baseboard_interrupt(u32 flag);

 private:
  struct Channel {
    u32 status = 0;
    u32 dma_address = 0;
    u32 dma_length = 0;
    u32 control = 0;
    u32 imm_data = 0;
    std::array<std::unique_ptr<ExiDevice>, 3> devices;  // CS 1, 2, 4
    ExiDevice* selected() const;
  };
  bool causing_interrupt(int index);
  void update_interrupts();

  std::array<Channel, 3> channels_;
  int log_remaining_ = 0;  // OPENGP_LOG_EXI=<n>: log the first n transfers
};

}  // namespace hw
