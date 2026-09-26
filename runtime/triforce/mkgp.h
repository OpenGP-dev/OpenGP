// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP / GP 2 cabinet: the force-feedback steering wheel on
// serial port A, and how the controls are wired to the JVS I/O board.
// Based on Dolphin's Triforce/MarioKartGP.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include "triforce/io_ports.h"
#include "triforce/serial_device.h"

namespace triforce {

class MkgpSteeringWheel final : public SerialDevice {
 public:
  void update() override;
  void reset() { init_state_ = 0; }

 private:
  u8 init_state_ = 0;
};

class MkgpIOAdapter final : public IOAdapter {
 public:
  MkgpIOAdapter(MkgpSteeringWheel* wheel, ControlsSource controls) : wheel_(wheel), controls_(std::move(controls)) {}
  void update(IOPorts& ports) override;
  void generic_outputs_changed(std::span<const u8> bits_set, std::span<const u8> bits_cleared) override;

 private:
  MkgpSteeringWheel* wheel_;
  ControlsSource controls_;
};

}  // namespace triforce
