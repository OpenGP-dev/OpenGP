// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/io_ports.h"

#include <algorithm>

namespace triforce {

IOPorts::IOPorts() = default;
IOPorts::~IOPorts() = default;

void IOPorts::update() {
  system_inputs_ = 0;
  switch_inputs_.fill(0);
  analog_inputs_.fill(kNeutralAnalog);
  coin_inputs_.fill(false);
  for (auto& adapter : adapters_) adapter->update(*this);
}

void IOPorts::set_generic_outputs(std::span<const u8> bytes) {
  const std::size_t count = std::min(bytes.size(), kGenericOutputBytes);
  std::array<u8, kGenericOutputBytes> bits_set{}, bits_cleared{};
  bool changed = false;
  for (std::size_t i = 0; i < count; ++i) {
    bits_set[i] = static_cast<u8>(~generic_outputs_[i] & bytes[i]);
    bits_cleared[i] = static_cast<u8>(generic_outputs_[i] & ~bytes[i]);
    changed |= bits_set[i] || bits_cleared[i];
    generic_outputs_[i] = bytes[i];
  }
  if (!changed) return;
  for (auto& adapter : adapters_) adapter->generic_outputs_changed(bits_set, bits_cleared);
}

void IOPorts::reset_generic_outputs() { set_generic_outputs(std::array<u8, kGenericOutputBytes>{}); }

void IOPorts::add_adapter(std::unique_ptr<IOAdapter> adapter) { adapters_.push_back(std::move(adapter)); }

void CommonIOAdapter::update(IOPorts& ports) {
  const Controls c = controls_ ? controls_() : Controls{};
  if (c.test) ports.system_inputs() |= 0x80;
  if (c.service) ports.switch_inputs(0)[0] |= 0x40;
  if (c.coin) ports.coin_inputs()[0] = true;
}

}  // namespace triforce
