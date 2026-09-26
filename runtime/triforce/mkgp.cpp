// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/mkgp.h"

namespace triforce {

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

void MkgpIOAdapter::update(IOPorts& ports) {
  const Controls c = controls_ ? controls_() : Controls{};
  const auto sw = ports.switch_inputs(0);
  if (c.start) sw[0] |= 0x80;
  if (c.item) sw[1] |= 0x20;
  if (c.cancel) sw[1] |= 0x02;  // VS cancel
  const auto analog = ports.analog_inputs();
  analog[0] = expand_analog(c.steering);
  analog[1] = expand_analog(c.gas);
  analog[2] = expand_analog(c.brake);
}

void MkgpIOAdapter::generic_outputs_changed(std::span<const u8> bits_set, std::span<const u8> bits_cleared) {
  (void)bits_cleared;
  // Bit 0x80 resets the wheel; 0x04 and 0x08 are the item and cancel button lamps.
  if (bits_set[0] & 0x80) wheel_->reset();
}

}  // namespace triforce
