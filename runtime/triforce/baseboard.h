// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AM-Baseboard as seen on SI port 0: board queries plus pass-through to
// the JVS I/O board and the two serial ports (A, B) that games use for their
// cabinet peripherals.
// Based on Dolphin's SI/SI_DeviceAMBaseboard.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <memory>
#include <string>

#include "hw/si.h"
#include "triforce/io_ports.h"
#include "triforce/jvsio.h"

namespace triforce {

class BaseboardSi final : public hw::SiDevice {
 public:
  // `game_id` picks the cabinet (I/O board and serial devices); cards are
  // stored in `user_dir`. `outputs` is told when the cabinet's outputs change.
  BaseboardSi(const std::string& game_id, const std::string& user_dir, ControlsSource controls,
              OutputsSink outputs = {});
  ~BaseboardSi() override;

  int run_buffer(u8* buffer, int request_length) override;
  hw::SiData get_data(u32&, u32&) override { return hw::SiData::NoData; }

 private:
  int run_baseboard_command(u8* buffer, int request_length);

  // Responses are returned one transfer late (the game's parser expects it).
  std::array<std::array<u8, hw::SiBus::kBufferSize>, 2> responses_{};
  std::size_t current_response_ = 0;

  IOPorts io_ports_;
  Outputs outputs_, reported_outputs_;
  OutputsSink outputs_sink_;
  std::unique_ptr<JvsIoBoard> jvs_;
  std::unique_ptr<SerialDevice> serial_a_, serial_b_;
};

}  // namespace triforce
