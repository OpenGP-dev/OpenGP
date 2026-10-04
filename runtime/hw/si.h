// SPDX-License-Identifier: GPL-3.0-or-later
//
// Serial interface (0xCC006400): four controller ports. Each port can run a
// command/response transfer through the 128-byte communication buffer, or be
// polled for controller data. On the Triforce, port 0 is the AM-Baseboard
// (JVS I/O and the cabinet's serial peripherals, see triforce/baseboard.h).
// Based on Dolphin's SI.cpp (Copyright Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <memory>

#include "hw/hardware.h"

namespace hw {

enum class SiData { Success, NoData, ErrorNoResponse };

class SiDevice {
 public:
  virtual ~SiDevice() = default;
  // Runs the command in `buffer` (always 128 bytes; the response replaces the
  // request). Returns the response length, 0 if the response is not ready yet
  // (the transfer is retried) or -1 if nothing answered.
  virtual int run_buffer(u8* buffer, int request_length) = 0;
  // Polling: the latest controller data.
  virtual SiData get_data(u32& hi, u32& lo) {
    (void)hi, (void)lo;
    return SiData::ErrorNoResponse;
  }
  virtual void send_command(u32 command, u8 poll) { (void)command, (void)poll; }
};

// An empty port.
class SiNullDevice : public SiDevice {
 public:
  int run_buffer(u8*, int) override { return -1; }
};

class SiBus : public Device {
 public:
  static constexpr u32 kBufferSize = 128;
  static constexpr int kChannels = 4;

  explicit SiBus(Hardware& hw);
  void attach(int channel, std::unique_ptr<SiDevice> device);
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return "SI"; }

 private:
  struct Channel {
    u32 out = 0, in_hi = 0, in_lo = 0;
    std::unique_ptr<SiDevice> device;
  };
  void run_transfer();
  void update_interrupts();

  std::array<Channel, kChannels> channels_;
  u32 poll_ = 492u << 16;  // X = 492 lines
  u32 com_csr_ = 0;
  u32 status_ = 0;
  u32 exi_clock_count_ = 0;
  std::array<u8, kBufferSize> buffer_{};
  Hardware::EventId retry_event_ = 0;
  int log_remaining_ = 0;  // OPENGP_LOG_SI=<n>: log the first n transfers
};

}  // namespace hw
