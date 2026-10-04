// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Triforce media board (DIMM board), which sits where the GameCube's
// disc drive would be. Games talk to it with obfuscated DI commands: reads
// from the game image (held in DIMM memory on real hardware), board status
// and configuration, and a command interface (Execute) that includes a
// socket API used for linking cabinets.
//
// Networking is not emulated yet: socket commands fail.
//
// Based on Dolphin's DVD/AMMediaboard.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "disc/disc.h"
#include "hw/di.h"

namespace hw {
class ExiBus;
}

namespace triforce {

using namespace gekko;

class MediaBoard final : public hw::DiBackend {
 public:
  MediaBoard(hw::Hardware& hw, const disc::Image& disc, const std::string& user_dir);
  ~MediaBoard() override;
  bool execute(std::array<u32, 3>& command, u32& immediate, u32 address, u32 length) override;

  // Sets the command obfuscation keys. The boot ROM leaves them in low
  // memory (the first 12 bytes of the disc header) after the apploader.
  void init_keys(u32 a, u32 b, u32 c) { key_a_ = a, key_b_ = b, key_c_ = c; }

 private:
  enum class Game { MarioKartGP, MarioKartGP2, Other };

  struct File {
    std::FILE* f = nullptr;
    void open(const std::string& path);
    void close();
    void read(u32 offset, u8* dst, u32 length);
    void write(u32 offset, const u8* src, u32 length);
  };

  struct Range {
    u32 start, size;
    u8* buffer;
    std::size_t buffer_size;
  };

  bool read(u32 offset, u32 address, u32 length);
  void write(u32 offset, u32 address, u32 length);
  void execute_board_command();  // DI Execute (protocol version 1)
  void execute1();               // DMA write to DIMMCommandExecute1
  void execute2(u32 address, u32 length);  // DMA read from DIMMCommandExecute2
  void socket_command(u16 command, u32 parameter_index);
  void schedule_test_result(bool execute2);

  std::span<u8> span_for(u32 offset);
  u8* guest(u32 address, u32 length);
  void raise_interrupt(u32 flag);

  // The media buffer holds little-endian data (the game byte-swaps).
  u32 mb32(u32 index) const;
  void set_mb32(u32 index, u32 value);
  u16 mb16_at(u32 byte_offset) const;

  hw::Hardware& hw_;
  const disc::Image& disc_;
  Game game_;
  u32 key_a_ = 0, key_b_ = 0, key_c_ = 0;

  File netcfg_, netctrl_, extra_, dimm_, backup_;

  std::array<u8, 0x300> media_buffer_{};
  std::array<u8, 0x20> exec1_response_{};
  std::array<u8, 0x20> exec2_response_{};
  std::vector<u8> network_command_buffer_;
  std::vector<u8> network_buffer_;
  std::vector<u8> allnet_buffer_;
  std::vector<u8> allnet_settings_;
  std::vector<Range> ranges_;

  u32 board_status_;
  u32 load_progress_ = 80;
  u32 last_error_;
  bool netconfig_read_pending_ = false;
  hw::Hardware::EventId test_event_ = 0;
};

}  // namespace triforce
