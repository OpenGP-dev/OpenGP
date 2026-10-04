// SPDX-License-Identifier: GPL-3.0-or-later
//
// DVD interface (0xCC006000): command registers, DMA and interrupts in front
// of the disc drive. On the Triforce the drive is replaced by the media board
// (triforce/mediaboard.h), which takes every command first.
// Based on Dolphin's DVDInterface.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <array>
#include <memory>

#include "disc/disc.h"
#include "hw/hardware.h"

namespace hw {

class DiBackend {
 public:
  virtual ~DiBackend() = default;
  // Runs a command (the three command words, which it may modify). Returns
  // false to let the drive handle the command instead.
  virtual bool execute(std::array<u32, 3>& command, u32& immediate, u32 address, u32 length) = 0;
};

class DiDevice : public Device {
 public:
  // `triforce`: the cover reads closed and the config register reports the
  // Triforce setting.
  DiDevice(Hardware& hw, const disc::Image& disc, bool triforce);
  void set_backend(std::unique_ptr<DiBackend> backend) { backend_ = std::move(backend); }
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "DI"; }

 private:
  void execute();
  bool drive_command();  // plain GameCube drive; false on error
  void update_interrupts();

  const disc::Image& disc_;
  std::unique_ptr<DiBackend> backend_;
  u32 sr_ = 0;   // status
  u32 cvr_ = 0;  // cover
  std::array<u32, 3> cmd_{};
  u32 mar_ = 0;     // DMA address
  u32 length_ = 0;  // DMA length
  u32 cr_ = 0;      // control
  u32 imm_ = 0;     // immediate data
  u32 cfg_ = 0;     // configuration (read-only)
  u32 error_ = 0;   // drive error code for RequestError
  int log_remaining_ = 0;  // OPENGP_LOG_DI=<n>: log the first n commands
};

}  // namespace hw
