// SPDX-License-Identifier: GPL-3.0-or-later
//
// A device on one of the AM-Baseboard's serial links (JVS, serial A/B): a
// byte stream in each direction. Based on Dolphin's Triforce/SerialDevice.h
// (Copyright Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <span>
#include <vector>

#include "gekko/types.h"

namespace triforce {

using namespace gekko;

class SerialDevice {
 public:
  SerialDevice() = default;
  virtual ~SerialDevice() = default;
  SerialDevice(const SerialDevice&) = delete;
  SerialDevice& operator=(const SerialDevice&) = delete;

  // Baseboard -> device.
  void write_rx(std::span<const u8> bytes) { rx_.insert(rx_.end(), bytes.begin(), bytes.end()); }

  // Device -> baseboard. `out.size()` must not exceed tx_count().
  std::size_t tx_count() const { return tx_.size(); }
  void take_tx(std::span<u8> out) {
    std::copy_n(tx_.begin(), out.size(), out.begin());
    tx_.erase(tx_.begin(), tx_.begin() + static_cast<std::ptrdiff_t>(out.size()));
  }

  // Processes whatever has been received.
  virtual void update() = 0;

 protected:
  std::span<const u8> rx() const { return rx_; }
  void consume_rx(std::size_t count) { rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(count)); }
  void write_tx(u8 byte) { tx_.push_back(byte); }
  void write_tx(std::span<const u8> bytes) { tx_.insert(tx_.end(), bytes.begin(), bytes.end()); }

 private:
  std::vector<u8> rx_;  // contiguous, for packet parsing
  std::deque<u8> tx_;   // may be read in chunks
};

}  // namespace triforce
