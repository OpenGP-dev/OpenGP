// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's SI/SI_DeviceAMBaseboard.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).

#include "triforce/baseboard.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string_view>

#include "triforce/magcard.h"
#include "triforce/mkgp.h"

namespace triforce {

namespace {

// SI commands.
constexpr u8 SI_CMD_STATUS = 0x00;
constexpr u8 SI_CMD_AM_BASEBOARD = 0x70;
constexpr u32 SI_AM_BASEBOARD = 0x10110800;  // ORed with the DIP switches

// Baseboard sub-commands inside an SI_CMD_AM_BASEBOARD request.
enum : u8 {
  GCAM_STATUS_SWITCHES = 0x10,
  GCAM_SERIAL_NUMBER = 0x11,
  GCAM_UNKNOWN_12 = 0x12,
  GCAM_UNKNOWN_14 = 0x14,
  GCAM_FIRM_VERSION = 0x15,
  GCAM_FPGA_VERSION = 0x16,
  GCAM_REGION_SETTINGS = 0x1F,
  GCAM_UNKNOWN_21 = 0x21,
  GCAM_UNKNOWN_22 = 0x22,
  GCAM_UNKNOWN_23 = 0x23,
  GCAM_UNKNOWN_24 = 0x24,
  GCAM_SERIAL_A = 0x31,
  GCAM_SERIAL_B = 0x32,
  GCAM_JVSIO_A = 0x40,
  GCAM_JVSIO_B = 0x41,
  GCAM_UNKNOWN_60 = 0x60,
};

// Serial port reads are limited to this many bytes per transfer.
constexpr u32 kSerialMaxRead = 0x1F;

// Region settings (USA).
constexpr u8 kRegionFlags[0x14] = {0x00, 0x00, 0x30, 0x00, 0x02, 0xFD, 0x00, 0x00, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

bool is_mario_kart(const std::string& game_id) {
  // GGPE01 (GP) and GGPE02 (GP 2).
  return game_id.rfind("GGPE", 0) == 0;
}

}  // namespace

BaseboardSi::BaseboardSi(const std::string& game_id, std::shared_ptr<CardFolder> cards, ControlsSource controls,
                         OutputsSink outputs)
    : outputs_sink_(std::move(outputs)) {
  io_ports_.add_adapter(std::make_unique<CommonIOAdapter>(controls));
  if (is_mario_kart(game_id)) {
    jvs_ = std::make_unique<NamcoFcaBoard>(&io_ports_);
    auto wheel = std::make_unique<MkgpSteeringWheel>(&outputs_);
    io_ports_.add_adapter(std::make_unique<MkgpIOAdapter>(wheel.get(), controls, &outputs_));
    serial_a_ = std::move(wheel);
    serial_b_ = std::make_unique<MagCardReader>(std::move(cards));
  } else {
    jvs_ = std::make_unique<Sega837_13551Board>(&io_ports_);
  }
}

BaseboardSi::~BaseboardSi() = default;

int BaseboardSi::run_buffer(u8* buffer, int request_length) {
  io_ports_.update();
  switch (buffer[0]) {
    case SI_CMD_STATUS: {
      // Device ID (3 bytes).
      const u32 id = SI_AM_BASEBOARD | 0x100;
      buffer[0] = static_cast<u8>(id >> 24);
      buffer[1] = static_cast<u8>(id >> 16);
      buffer[2] = static_cast<u8>(id >> 8);
      return 3;
    }
    case SI_CMD_AM_BASEBOARD: return run_baseboard_command(buffer, request_length);
    default: std::fprintf(stderr, "[baseboard] unknown SI command %02X\n", buffer[0]); return -1;
  }
}

int BaseboardSi::run_baseboard_command(u8* buffer, int request_length) {
  const int command_length = buffer[1];
  if (2 + command_length > request_length) {
    std::fprintf(stderr, "[baseboard] bad command length %d for request length %d\n", command_length, request_length);
    return -1;
  }

  auto& out = responses_[current_response_];
  const u32 usable = static_cast<u32>(out.size()) - 1;  // room for the checksum
  u32 offset = 2;                                      // after the response header
  const u8* in = buffer + 2;
  const u8* const in_end = in + command_length;

  // Checks that `n_in` more request bytes exist and `n_out` response bytes fit;
  // otherwise gives up on the rest of the request.
  auto fits = [&](u32 n_in, u32 n_out, const char* what) {
    if (in + n_in <= in_end && offset + n_out <= usable) return true;
    std::fprintf(stderr, "[baseboard] overflow in %s\n", what);
    in = in_end;
    return false;
  };
  auto put = [&](u8 v) { out[offset++] = v; };

  // Reads a serial device's pending output into the response.
  auto drain = [&](SerialDevice* device, u8 command, u32 max) {
    if (!device) return;
    const u32 length = std::min(static_cast<u32>(device->tx_count()), max);
    if (length == 0 || !fits(0, length + 2, "serial read")) return;
    put(command);
    put(static_cast<u8>(length));
    device->take_tx(std::span{out}.subspan(offset, length));
    offset += length;
  };

  while (in < in_end) {
    const u8 command = *in++;
    switch (command) {
      case GCAM_STATUS_SWITCHES: {
        const auto switches = io_ports_.status_switches();
        if (!fits(1, 2 + static_cast<u32>(switches.size()), "status switches")) break;
        ++in;
        put(command);
        put(static_cast<u8>(switches.size()));
        for (const u8 v : switches) put(v);
        break;
      }
      case GCAM_SERIAL_NUMBER:
        if (!fits(1, 18, "serial number")) break;
        ++in;
        put(command);
        put(16);
        for (const char ch : std::string_view("AADE-01B98394904")) put(static_cast<u8>(ch));
        break;
      case GCAM_UNKNOWN_12:
      case GCAM_UNKNOWN_14:
      case GCAM_UNKNOWN_23:
      case GCAM_UNKNOWN_24:
        if (!fits(2, 2, "unknown")) break;
        in += 2;
        put(command);
        put(0);
        break;
      case GCAM_FIRM_VERSION:
        if (!fits(1, 4, "firmware version")) break;
        ++in;
        put(command);
        put(2);
        put(0x00);  // 00.26
        put(0x26);
        break;
      case GCAM_FPGA_VERSION:
        if (!fits(1, 4, "FPGA version")) break;
        ++in;
        put(command);
        put(2);
        put(0x07);  // 07.06
        put(0x06);
        break;
      case GCAM_REGION_SETTINGS:
        if (!fits(5, 0x16, "region")) break;
        in += 5;
        put(command);
        put(sizeof(kRegionFlags));
        for (const u8 v : kRegionFlags) put(v);
        break;
      case GCAM_UNKNOWN_21:  // no reply
        if (!fits(4, 0, "unknown 21")) break;
        in += 4;
        break;
      case GCAM_UNKNOWN_22:  // no reply
        if (!fits(7, 0, "unknown 22") || !fits(in[0] + 1u, 0, "unknown 22")) break;
        in += in[0] + 1u;
        break;
      case GCAM_SERIAL_A:
      case GCAM_SERIAL_B: {
        if (!fits(1, 0, "serial")) break;
        const u32 length = *in++;
        if (!fits(length, 0, "serial")) break;
        SerialDevice* device = command == GCAM_SERIAL_A ? serial_a_.get() : serial_b_.get();
        if (device) device->write_rx({in, length});
        in += length;
        break;
      }
      case GCAM_JVSIO_A:
      case GCAM_JVSIO_B: {
        if (!fits(1, 0, "JVS")) break;
        const u32 length = *in++;
        if (!fits(length, 0, "JVS")) break;
        jvs_->write_rx({in, length});
        in += length;
        jvs_->update();
        drain(jvs_.get(), command, 0xFF);
        break;
      }
      case GCAM_UNKNOWN_60:  // no reply
        if (!fits(3, 0, "unknown 60") || !fits(in[0] + 1u, 0, "unknown 60")) break;
        in += in[0] + 1u;
        break;
      default:
        std::fprintf(stderr, "[baseboard] unknown command %02X\n", command);
        in = in_end;
        break;
    }
  }

  // The serial devices are polled on every transfer.
  if (serial_a_) serial_a_->update();
  drain(serial_a_.get(), GCAM_SERIAL_A, kSerialMaxRead);
  if (serial_b_) serial_b_->update();
  drain(serial_b_.get(), GCAM_SERIAL_B, kSerialMaxRead);

  if (outputs_sink_ && outputs_ != reported_outputs_) {
    reported_outputs_ = outputs_;
    outputs_sink_(outputs_);
  }

  out[0] = 0x01;  // status
  out[1] = static_cast<u8>(offset - 2);
  std::fill(out.begin() + offset, out.begin() + usable, u8{0});
  out[usable] = static_cast<u8>(~std::accumulate(out.begin(), out.begin() + offset, u8{0}));

  // Return the previous response.
  current_response_ ^= 1;
  const auto& response = responses_[current_response_];
  if (response[0] == 0) return 0;  // nothing yet: the transfer is retried
  std::copy(response.begin(), response.end(), buffer);
  return static_cast<int>(response.size());
}

}  // namespace triforce
