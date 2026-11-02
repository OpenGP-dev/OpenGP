// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's Triforce/MarioKartGP.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).

#include "triforce/mkgp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace triforce {

namespace {

// OPENGP_LOG_FFB=1 logs the forces the game sends to the wheel.
bool ffb_logging() {
  static const bool enabled = std::getenv("OPENGP_LOG_FFB") != nullptr;
  return enabled;
}

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }

}  // namespace

void MkgpSteeringWheel::update() {
  // Requests are 10 bytes: FF FF FF 01, then centering force, friction force
  // and roll (16 bits each).
  constexpr std::size_t kRequestSize = 10;
  static constexpr u8 kAckNoError[] = {'E', '0', '0'};
  static constexpr u8 kAckPowerOn[] = {'C', '0', '1'};
  static constexpr u8 kAckPowerOff[] = {'C', '0', '6'};

  std::size_t pos = 0;
  while (rx().size() - pos >= kRequestSize) {
    const auto request = rx().subspan(pos, kRequestSize);
    pos += kRequestSize;
    if (request[0] != 0xFF || request[1] != 0xFF || request[2] != 0xFF || request[3] != 0x01) continue;
    const u16 centering = be16(&request[4]), friction = be16(&request[6]), roll = be16(&request[8]);
    if (ffb_logging() && (centering != outputs_->centering || friction != outputs_->friction || roll != outputs_->roll)) {
      std::fprintf(stderr, "[ffb] centering %04X friction %04X roll %04X\n", centering, friction, roll);
    }
    outputs_->centering = centering;
    outputs_->friction = friction;
    outputs_->roll = roll;
    if (init_state_ == 0) {
      // Expected at power-up.
      write_tx(kAckNoError);
      init_state_ = 1;
    } else if (init_state_ == 1) {
      // The game only sends forces after seeing "power on", and after a race
      // lowers them to 0 and waits for "power off". Alternating keeps it happy.
      write_tx(kAckPowerOff);
      init_state_ = 2;
    } else {
      write_tx(kAckPowerOn);
      init_state_ = 1;
    }
  }
  consume_rx(pos);
}

void MkgpSteeringWheel::reset() {
  init_state_ = 0;
  outputs_->centering = outputs_->friction = outputs_->roll = 0;
}

void MkgpIOAdapter::update(IOPorts& ports) {
  const Controls c = controls_ ? controls_() : Controls{};
  const auto sw = ports.switch_inputs(0);
  if (c.start) sw[0] |= 0x80;
  if (c.item) sw[1] |= 0x20;
  if (c.cancel) sw[1] |= 0x02;  // VS cancel
  // The game calibrates the wheel and pedals from its settings (defaults:
  // steering centred at 0x7FFF, pedals at rest at 0): steering reaches full
  // lock 0x5000 either side of the centre, and a pedal reads from 0x800 above
  // its rest position to 0x4000 further. Scale the controls to exactly that,
  // with a little to spare, so that all of their travel is used.
  const auto analog = ports.analog_inputs();
  const long steer = std::lround(std::clamp(c.steering, -1.0f, 1.0f) * 0x5200);
  analog[0] = static_cast<u16>(std::clamp(0x8000L + steer, 0L, 0xFFFFL));
  auto pedal = [](float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return static_cast<u16>(v > 0.0f ? 0x800 + std::lround(v * 0x4100) : 0);
  };
  analog[1] = pedal(c.gas);
  analog[2] = pedal(c.brake);
}

void MkgpIOAdapter::generic_outputs_changed(std::span<const u8> bits_set, std::span<const u8> bits_cleared) {
  // Bit 0x80 resets the wheel; 0x04 and 0x08 are the item and cancel button lamps.
  if (bits_set[0] & 0x80) wheel_->reset();
  if (bits_set[0] & 0x04) outputs_->item_lamp = true;
  if (bits_cleared[0] & 0x04) outputs_->item_lamp = false;
  if (bits_set[0] & 0x08) outputs_->cancel_lamp = true;
  if (bits_cleared[0] & 0x08) outputs_->cancel_lamp = false;
}

}  // namespace triforce
