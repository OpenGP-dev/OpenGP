// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/jvsio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <optional>

namespace triforce {

namespace {

constexpr u8 JVS_SYNC = 0xE0;
constexpr u8 JVS_MARK = 0xD0;  // escape: the next byte is value - 1
constexpr u8 JVS_BROADCAST = 0xFF;
constexpr u8 JVS_HOST = 0x00;

enum Command : u8 {
  CMD_IO_IDENTIFY = 0x10,
  CMD_COMMAND_REVISION = 0x11,
  CMD_JVS_REVISION = 0x12,
  CMD_COMM_VERSION = 0x13,
  CMD_FEATURE_CHECK = 0x14,
  CMD_MAIN_ID = 0x15,
  CMD_SWITCH_INPUT = 0x20,
  CMD_COIN_INPUT = 0x21,
  CMD_ANALOG_INPUT = 0x22,
  CMD_DATA_RETRANSMIT = 0x2F,
  CMD_COIN_COUNTER_DEC = 0x30,
  CMD_GENERIC_OUTPUT = 0x32,
  CMD_COIN_COUNTER_INC = 0x35,
  CMD_NAMCO = 0x70,
  CMD_RESET = 0xF0,
  CMD_SET_ADDRESS = 0xF1,
};

// Feature codes for FeatureCheck.
enum Feature : u8 {
  FEAT_SWITCH_INPUT = 0x01,  // players, buttons, 0
  FEAT_COIN_INPUT = 0x02,    // slots, 0, 0
  FEAT_ANALOG_INPUT = 0x03,  // channels, bits, 0
  FEAT_GENERAL_PURPOSE_OUTPUT = 0x12,  // slots, 0, 0
  FEAT_ANALOG_OUTPUT = 0x13,  // channels, 0, 0
};

enum CoinCondition : u8 { COIN_NORMAL = 0, COIN_JAM = 1, COIN_COUNTER_DISCONNECTED = 2, COIN_BUSY = 3 };

// Decodes exactly output.size() bytes; returns how many escaped bytes were used.
std::optional<std::size_t> unescape(std::span<const u8> input, std::span<u8> output) {
  std::size_t in = 0;
  for (u8& out : output) {
    if (in == input.size()) return std::nullopt;
    u8 v = input[in++];
    if (v == JVS_MARK) {
      if (in == input.size()) return std::nullopt;
      v = static_cast<u8>(input[in++] + 1);
    }
    out = v;
  }
  return in;
}

// OPENGP_LOG_JVS=1 logs the protocol.
bool jvs_logging() {
  static const bool enabled = std::getenv("OPENGP_LOG_JVS") != nullptr;
  return enabled;
}
#define log_jvs(...)                     \
  do {                                   \
    if (jvs_logging()) {                 \
      std::fputs("[jvs] ", stderr);      \
      std::fprintf(stderr, __VA_ARGS__); \
      std::fputc('\n', stderr);          \
    }                                    \
  } while (0)

}  // namespace

class JvsRequest {
 public:
  explicit JvsRequest(std::span<const u8> data) : data_(data) {}
  std::span<const u8> peek() const { return data_.subspan(pos_); }
  std::size_t remaining() const { return data_.size() - pos_; }
  u8 read() { return data_[pos_++]; }
  void skip(std::size_t n) { pos_ += n; }

 private:
  std::span<const u8> data_;
  std::size_t pos_ = 0;
};

class JvsResponse {
 public:
  explicit JvsResponse(std::vector<u8>* buffer) : buffer_(*buffer) {}
  void add(u8 v) { buffer_.push_back(v); }
  void add(std::span<const u8> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }
  void add_be16(u16 v) {
    add(static_cast<u8>(v >> 8));
    add(static_cast<u8>(v));
  }
  void add_string(std::string_view s) {  // including the terminator
    add(std::span{reinterpret_cast<const u8*>(s.data()), s.size()});
    add(0);
  }
  void start_frame(u8 destination) {
    buffer_.clear();
    add(destination);
    add(0);  // byte count, filled in by end_frame
    add(0);  // status
  }
  void end_frame(JvsIoBoard::Status status) {
    std::size_t count = buffer_.size() - 1;  // including the checksum
    if (count > 255) {
      status = JvsIoBoard::Status::ResponseOverflow;
      buffer_.resize(3);
      count = 2;
    }
    buffer_[1] = static_cast<u8>(count);
    buffer_[2] = static_cast<u8>(status);
    add(std::accumulate(buffer_.begin(), buffer_.end(), u8{0}));
  }
  void start_report() {
    report_index_ = buffer_.size();
    add(0);
  }
  void set_report(JvsIoBoard::Report code) { buffer_[report_index_] = static_cast<u8>(code); }

 private:
  std::vector<u8>& buffer_;
  std::size_t report_index_ = 0;
};

void JvsIoBoard::update() {
  // Count coin insertions (rising edges).
  const auto coins = io_ports_->coin_inputs();
  for (std::size_t i = 0; i < coin_counts_.size(); ++i) {
    if (coins[i] && !coin_prev_[i]) ++coin_counts_[i];
    coin_prev_[i] = coins[i];
  }

  for (;;) {
    const auto data = rx();
    if (data.empty()) break;
    if (data[0] != JVS_SYNC) {
      log_jvs("expected sync, got %02X", data[0]);
      consume_rx(1);
      continue;
    }
    std::array<u8, 2 + 255> frame{};
    const auto header_size = unescape(data.subspan(1), std::span{frame}.first(2));
    if (!header_size) break;  // wait for more data
    const u8 destination = frame[0];
    const u8 payload_size = frame[1];
    const auto payload_escaped = unescape(data.subspan(1 + *header_size), std::span{frame}.subspan(2, payload_size));
    if (!payload_escaped) break;
    consume_rx(1 + *header_size + *payload_escaped);

    if (payload_size < 1) continue;
    const auto checked = std::span{frame}.first(payload_size + 1u);
    if (std::accumulate(checked.begin(), checked.end(), u8{0}) != frame[payload_size + 1u]) {
      log_jvs("bad checksum");
      JvsResponse response{&last_response_};
      response.start_frame(JVS_HOST);
      response.end_frame(Status::ChecksumError);
      write_response(last_response_);
      continue;
    }
    JvsRequest request{checked.subspan(2)};
    if (destination == JVS_BROADCAST) process_broadcast(request);
    else if (destination == client_address_) process_unicast(request);
    else log_jvs("frame for node %02X ignored", destination);
  }
}

void JvsIoBoard::process_broadcast(JvsRequest& request) {
  if (request.remaining() < 2) return;
  const u8 command = request.read();
  switch (command) {
    case CMD_RESET:
      if (request.read() == 0xD9) {
        log_jvs("reset");
        client_address_ = 0;
        io_ports_->status_switches()[1] |= 0x01;  // sense line: not addressed
      }
      io_ports_->reset_generic_outputs();
      last_response_.clear();
      break;
    case CMD_SET_ADDRESS: {
      client_address_ = request.read();  // no daisy chaining
      log_jvs("address %u", client_address_);
      io_ports_->status_switches()[1] &= ~0x01;
      JvsResponse response{&last_response_};
      response.start_frame(JVS_HOST);
      response.add(static_cast<u8>(Report::Okay));
      response.end_frame(Status::Okay);
      write_response(last_response_);
      break;
    }
    default: log_jvs("unknown broadcast command %02X", command); break;
  }
}

void JvsIoBoard::process_unicast(JvsRequest& request) {
  if (request.remaining() > 0 && request.peek()[0] == CMD_DATA_RETRANSMIT) {
    request.skip(1);
    write_response(last_response_);
    return;
  }
  JvsResponse response{&last_response_};
  response.start_frame(JVS_HOST);
  for (;;) {
    if (request.remaining() == 0) {
      response.end_frame(Status::Okay);
      break;
    }
    response.start_report();
    const u8 command = request.read();
    const Result result = handle_command(command, request, response);
    if (result.frame_error) {
      log_jvs("command %02X: status %u", command, static_cast<u32>(result.status));
      response.end_frame(result.status);
      break;
    }
    response.set_report(result.report);
    if (result.report != Report::Okay) {
      log_jvs("command %02X: report %u", command, static_cast<u32>(result.report));
      response.end_frame(Status::Okay);
      break;
    }
  }
  write_response(last_response_);
}

void JvsIoBoard::write_response(std::span<const u8> response) {
  write_tx(JVS_SYNC);
  for (const u8 v : response) {
    if (v == JVS_SYNC || v == JVS_MARK) {
      write_tx(JVS_MARK);
      write_tx(static_cast<u8>(v - 1));
    } else {
      write_tx(v);
    }
  }
}

JvsIoBoard::Result JvsIoBoard::handle_command(u8 command, JvsRequest& request, JvsResponse& response) {
  switch (command) {
    case CMD_COMMAND_REVISION: response.add(0x11); return Report::Okay;
    case CMD_JVS_REVISION: response.add(0x20); return Report::Okay;
    case CMD_COMM_VERSION: response.add(0x10); return Report::Okay;
    case CMD_MAIN_ID: {
      const auto rest = request.peek();
      const auto end = std::find(rest.begin(), rest.end(), u8{0});
      if (end == rest.end()) return Report::ParameterDataError;
      log_jvs("main ID: %.*s", static_cast<int>(end - rest.begin()), reinterpret_cast<const char*>(rest.data()));
      request.skip(static_cast<std::size_t>(end - rest.begin()) + 1);
      return Report::Okay;
    }
    case CMD_SWITCH_INPUT: {
      if (request.remaining() < 2) return Report::ParameterSizeError;
      const u8 players = request.read();
      const u8 bytes_per_player = request.read();
      response.add(io_ports_->system_inputs());
      for (u8 p = 0; p < players; ++p) {
        for (u8 i = 0; i < bytes_per_player; ++i) {
          const bool have = p < IOPorts::kPlayers && i < io_ports_->switch_inputs(0).size();
          response.add(have ? io_ports_->switch_inputs(p)[i] : u8{0});
        }
      }
      return Report::Okay;
    }
    case CMD_ANALOG_INPUT: {
      if (request.remaining() < 1) return Report::ParameterSizeError;
      const u8 channels = request.read();
      const auto analog = io_ports_->analog_inputs();
      for (u8 i = 0; i < channels; ++i) response.add_be16(i < analog.size() ? analog[i] : IOPorts::kNeutralAnalog);
      return Report::Okay;
    }
    case CMD_GENERIC_OUTPUT: {
      if (request.remaining() < 1) return Report::ParameterSizeError;
      const u8 count = request.read();
      if (request.remaining() < count) return Report::ParameterSizeError;
      io_ports_->set_generic_outputs(request.peek().first(count));
      request.skip(count);
      return Report::Okay;
    }
    case CMD_COIN_INPUT: {
      if (request.remaining() < 1) return Report::ParameterSizeError;
      const u8 slots = request.read();
      for (u8 i = 0; i < slots; ++i) {
        // Top 2 bits: condition; bottom 14 bits: coin count.
        const u16 v = i < coin_counts_.size() ? static_cast<u16>((COIN_NORMAL << 14) | (coin_counts_[i] & 0x3FFF))
                                              : static_cast<u16>(COIN_COUNTER_DISCONNECTED << 14);
        response.add_be16(v);
      }
      return Report::Okay;
    }
    case CMD_COIN_COUNTER_DEC:
    case CMD_COIN_COUNTER_INC: {
      if (request.remaining() < 3) return Report::ParameterSizeError;
      const u8 slot = request.read();
      const int msb = request.read();
      const int lsb = request.read();
      const int adjust = ((msb << 8) | lsb) * (command == CMD_COIN_COUNTER_DEC ? -1 : 1);
      log_jvs("coin counter %u: %+d", slot, adjust);
      if (slot < coin_counts_.size()) coin_counts_[slot] = static_cast<u16>(std::clamp(coin_counts_[slot] + adjust, 0, 0x3FFF));
      return Report::Okay;
    }
    default: return Result::error(Status::UnknownCommand);
  }
}

// JVS board descriptions from https://www.arcade-projects.com/threads/jvs-information-extractor.2071/

JvsIoBoard::Result NamcoFcaBoard::handle_command(u8 command, JvsRequest& request, JvsResponse& response) {
  switch (command) {
    case CMD_IO_IDENTIFY: response.add_string("namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder"); return Report::Okay;
    case CMD_FEATURE_CHECK: {
      // Rotary input is left out so that the RotaryInput command is not needed.
      static constexpr u8 kFeatures[] = {
          FEAT_SWITCH_INPUT, 1, 16, 0,        //
          FEAT_COIN_INPUT, 2, 0, 0,           //
          FEAT_ANALOG_INPUT, 7, 0, 0,         //
          FEAT_GENERAL_PURPOSE_OUTPUT, 6, 0, 0,  //
          FEAT_ANALOG_OUTPUT, 4, 0, 0,        //
          0,                                  // end
      };
      response.add(kFeatures);
      return Report::Okay;
    }
    case CMD_NAMCO: {
      if (request.remaining() < 1) return Report::ParameterSizeError;
      const u8 sub = request.read();
      if (sub != 0x18) {
        log_jvs("unknown Namco command %02X", sub);
        return Report::ParameterDataError;
      }
      if (request.remaining() < 4) return Report::ParameterSizeError;
      request.skip(4);
      response.add(0xFF);
      return Report::Okay;
    }
    default: return JvsIoBoard::handle_command(command, request, response);
  }
}

JvsIoBoard::Result Sega837_13551Board::handle_command(u8 command, JvsRequest& request, JvsResponse& response) {
  switch (command) {
    case CMD_IO_IDENTIFY: response.add_string("SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10"); return Report::Okay;
    case CMD_FEATURE_CHECK: {
      static constexpr u8 kFeatures[] = {
          FEAT_SWITCH_INPUT, 2, 13, 0,           //
          FEAT_COIN_INPUT, 2, 0, 0,              //
          FEAT_ANALOG_INPUT, 8, 0, 0,            //
          FEAT_GENERAL_PURPOSE_OUTPUT, 6, 0, 0,  //
          0,                                     // end
      };
      response.add(kFeatures);
      return Report::Okay;
    }
    default: return JvsIoBoard::handle_command(command, request, response);
  }
}

}  // namespace triforce
