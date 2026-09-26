// SPDX-License-Identifier: GPL-3.0-or-later
//
// JVS ("JAMMA Video Standard") I/O board: switches, coins, analog inputs and
// generic outputs over a framed, escaped serial protocol.
// Based on Dolphin's Triforce/JVSIO.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

#include "triforce/io_ports.h"
#include "triforce/serial_device.h"

namespace triforce {

class JvsRequest;
class JvsResponse;

class JvsIoBoard : public SerialDevice {
 public:
  enum class Status : u8 { Okay = 1, UnknownCommand = 2, ChecksumError = 3, ResponseOverflow = 4 };
  enum class Report : u8 { Okay = 1, ParameterSizeError = 2, ParameterDataError = 3, Busy = 4 };

  explicit JvsIoBoard(IOPorts* io_ports) : io_ports_(io_ports) {}
  void update() override;

 protected:
  // A command either produces a report code, or fails the whole frame.
  struct Result {
    bool frame_error = false;
    Report report = Report::Okay;
    Status status = Status::Okay;
    Result(Report r) : report(r) {}  // NOLINT(google-explicit-constructor)
    static Result error(Status s) {
      Result r{Report::Okay};
      r.frame_error = true;
      r.status = s;
      return r;
    }
  };
  virtual Result handle_command(u8 command, JvsRequest& request, JvsResponse& response);

 private:
  void process_broadcast(JvsRequest& request);
  void process_unicast(JvsRequest& request);
  void write_response(std::span<const u8> response);

  u8 client_address_ = 0;  // 0: not assigned yet
  std::vector<u8> last_response_;  // without the sync byte
  std::array<u16, IOPorts::kCoinSlots> coin_counts_{};
  std::array<bool, IOPorts::kCoinSlots> coin_prev_{};
  IOPorts* const io_ports_;
};

// Namco FCA-1, used by Mario Kart Arcade GP and GP 2.
class NamcoFcaBoard final : public JvsIoBoard {
 public:
  using JvsIoBoard::JvsIoBoard;
  Result handle_command(u8 command, JvsRequest& request, JvsResponse& response) override;
};

// Sega 837-13551, the usual Sega board.
class Sega837_13551Board final : public JvsIoBoard {
 public:
  using JvsIoBoard::JvsIoBoard;
  Result handle_command(u8 command, JvsRequest& request, JvsResponse& response) override;
};

}  // namespace triforce
