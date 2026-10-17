// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's DVD/AMMediaboard.cpp (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).

#include "triforce/mediaboard.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "hw/exi.h"

namespace triforce {

namespace {

// DI commands (low byte of the decoded first command word).
enum : u8 {
  DI_INQUIRY = 0x12,
  DI_READ = 0xA8,
  DI_WRITE = 0xAA,
  DI_EXECUTE = 0xAB,
};

// Inquiry replies select the command protocol version.
constexpr u32 INQUIRY_VERSION1 = 0x21484100;
constexpr u32 INQUIRY_VERSION2 = 0x29484100;

// Board commands (Execute).
enum : u16 {
  CMD_UNKNOWN_000 = 0x000,
  CMD_GET_DIMM_SIZE = 0x001,
  CMD_GET_MEDIA_BOARD_STATUS = 0x100,
  CMD_GET_SEGABOOT_VERSION = 0x101,  // GetNetworkFirmVersion on Execute2
  CMD_GET_SYSTEM_FLAGS = 0x102,
  CMD_GET_MEDIA_BOARD_SERIAL = 0x103,
  CMD_GET_NETWORK_CONFIG = 0x104,
  CMD_NETWORK_REINIT = 0x204,
  CMD_TEST_HARDWARE = 0x301,
  // 0x401-0x415: sockets (NET_* in network.h)
  // F-Zero AX link
  CMD_INIT_LINK = 0x601,
  CMD_ALLNET_INIT = 0x700,
};

// Media board address space.
enum : u32 {
  MB_STATUS1 = 0x80000000,
  MB_STATUS2 = 0x80000020,
  MB_STATUS3 = 0x80000040,
  FIRMWARE_STATUS1 = 0x80000120,
  FIRMWARE_STATUS2 = 0x80000140,
  BACKUP_MEMORY = 0x000006A0,
  DIMM_MEMORY = 0x1F000000,
  DIMM_MEMORY2 = 0xFF000000,
  DIMM_EXTRA_SETTINGS = 0x1FFEFFE0,
  ALLNET_SETTINGS = 0x1EFF8000,
  NETWORK_CONTROL = 0xFFFF0000,
  DIMM_COMMAND_VERSION1 = 0x1F900000,
  DIMM_COMMAND_VERSION2 = 0x84000000,
  DIMM_COMMAND_VERSION2_2 = 0x89000000,
  DIMM_COMMAND_EXECUTE1 = 0x84000040,
  DIMM_COMMAND_EXECUTE2 = 0x88000000,
  NETWORK_COMMAND_ADDRESS1 = 0x1F800200,
  NETWORK_COMMAND_ADDRESS2 = 0x89040200,
  NETWORK_BUFFER_ADDRESS1 = 0x1FA00000,
  NETWORK_BUFFER_ADDRESS2 = 0x1FD00000,
  NETWORK_BUFFER_ADDRESS3 = 0x89100000,
  NETWORK_BUFFER_ADDRESS4 = 0x89180000,
  NETWORK_BUFFER_ADDRESS5 = 0x1FB00000,
  ALLNET_BUFFER = 0x89010000,
  FIRMWARE_ADDRESS = 0x84800000,
  FIRMWARE_MAGIC_WRITE1 = 0x00600000,
  FIRMWARE_MAGIC_WRITE2 = 0x00700000,
  MAX_DISC_OFFSET = 0x57058000,
};

enum : u32 { STATUS_LOADING_GAME_PROGRAM = 4, STATUS_LOADED_GAME_PROGRAM = 5 };
enum : u8 { MEDIA_GDROM = 1, BOARD_NAND_MASK_NAND = 2 };

// "TEST OK", read by the game's display code with a byte swap.
constexpr u32 TEST_OK_WORD0 = 0x54455354;
constexpr u32 TEST_OK_WORD1 = 0x204F4B00;

constexpr char kAllNetReply[] =
    "uri=http://sega.com&host=sega.com&nickname=sega&name=sega&year=2025&month=08&day=16&hour=21&minute=10&"
    "second=12&place_id=1234&setting=0x123&region0=jap&region_name0=japan&region_name1=usa&region_"
    "name2=asia&region_name3=export&end";

void put_be32(u8* p, u32 v) {
  p[0] = static_cast<u8>(v >> 24), p[1] = static_cast<u8>(v >> 16), p[2] = static_cast<u8>(v >> 8), p[3] = static_cast<u8>(v);
}
void put_le32(u8* p, u32 v) {
  p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8), p[2] = static_cast<u8>(v >> 16), p[3] = static_cast<u8>(v >> 24);
}
u32 get_be32(const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; }

bool log_enabled() {
  static const bool enabled = std::getenv("OPENGP_LOG_MEDIABOARD") != nullptr;
  return enabled;
}

}  // namespace

#define mb_log(...)                           \
  do {                                        \
    if (log_enabled()) {                      \
      std::fputs("[mediaboard] ", stderr);    \
      std::fprintf(stderr, __VA_ARGS__);      \
      std::fputc('\n', stderr);               \
    }                                         \
  } while (0)
#define mb_error(...)                      \
  do {                                     \
    std::fputs("[mediaboard] ", stderr);   \
    std::fprintf(stderr, __VA_ARGS__);     \
    std::fputc('\n', stderr);              \
  } while (0)

// ---- Files -------------------------------------------------------------------------------------

void MediaBoard::File::open(const std::string& path) {
  f = std::fopen(path.c_str(), "rb+");
  if (!f) f = std::fopen(path.c_str(), "wb+");
  if (!f) mb_error("cannot open %s", path.c_str());
}

void MediaBoard::File::close() {
  if (f) std::fclose(f);
  f = nullptr;
}

void MediaBoard::File::read(u32 offset, u8* dst, u32 length) {
  std::memset(dst, 0, length);
  if (!f) return;
  std::fseek(f, static_cast<long>(offset), SEEK_SET);
  (void)std::fread(dst, 1, length, f);
}

void MediaBoard::File::write(u32 offset, const u8* src, u32 length) {
  if (!f) return;
  std::fseek(f, static_cast<long>(offset), SEEK_SET);
  std::fwrite(src, 1, length, f);
  std::fflush(f);
}

// ---- Board -------------------------------------------------------------------------------------

MediaBoard::MediaBoard(hw::Hardware& hw, const disc::Image& disc, const std::string& user_dir)
    : hw_(hw),
      disc_(disc),
      network_command_buffer_(0x4FFE00),
      network_buffer_(0x80000),
      allnet_buffer_(0x1000),
      allnet_settings_(0x8500),
      board_status_(STATUS_LOADING_GAME_PROGRAM),
      network_([this](u32 address) { return span_for(address); }) {
  const std::string& id = disc.game_id();
  game_ = id == "GGPE01" ? Game::MarioKartGP : id == "GGPE02" ? Game::MarioKartGP2 : Game::Other;

  std::error_code ec;
  std::filesystem::create_directories(user_dir, ec);
  netcfg_.open(user_dir + "/trinetcfg.bin");
  netctrl_.open(user_dir + "/trinetctrl.bin");
  extra_.open(user_dir + "/triextra.bin");
  dimm_.open(user_dir + "/tridimm_" + id + ".bin");
  backup_.open(user_dir + "/backup_" + id + ".bin");

  auto add = [this](u32 start, u32 size, std::span<u8> buffer) {
    ranges_.push_back({start, std::min<u32>(size, static_cast<u32>(buffer.size())), buffer.data(), buffer.size()});
  };
  add(DIMM_COMMAND_VERSION1, 0x40, media_buffer_);
  add(DIMM_COMMAND_VERSION2, 0x60, media_buffer_);
  add(DIMM_COMMAND_VERSION2_2, 0x220, media_buffer_);
  add(NETWORK_COMMAND_ADDRESS1, 0x1040, network_command_buffer_);
  add(NETWORK_COMMAND_ADDRESS2, 0x40000, network_command_buffer_);
  add(NETWORK_BUFFER_ADDRESS1, 0x10000, network_buffer_);
  add(NETWORK_BUFFER_ADDRESS2, 0x10000, network_buffer_);
  add(NETWORK_BUFFER_ADDRESS3, 0x50000, network_buffer_);
  add(NETWORK_BUFFER_ADDRESS4, 0xC0000, network_buffer_);
  add(NETWORK_BUFFER_ADDRESS5, 0x10000, network_buffer_);
  add(ALLNET_SETTINGS, 0x8000, allnet_settings_);
  add(ALLNET_BUFFER, 0x1000, allnet_buffer_);
}

MediaBoard::~MediaBoard() {
  for (File* f : {&netcfg_, &netctrl_, &extra_, &dimm_, &backup_}) f->close();
}

u32 MediaBoard::mb32(u32 index) const {
  const u8* p = &media_buffer_[index * 4];
  return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}

void MediaBoard::set_mb32(u32 index, u32 value) { put_le32(&media_buffer_[index * 4], value); }

u16 MediaBoard::mb16_at(u32 byte_offset) const {
  return static_cast<u16>(media_buffer_[byte_offset] | (media_buffer_[byte_offset + 1] << 8));
}

std::span<u8> MediaBoard::span_for(u32 offset) {
  for (const Range& r : ranges_) {
    if (offset >= r.start && offset < r.start + r.size) return std::span{r.buffer, r.buffer_size}.subspan(offset - r.start);
  }
  return {};
}

u8* MediaBoard::guest(u32 address, u32 length) { return hw_.ram(address, std::max<u32>(length, 1)); }

void MediaBoard::raise_interrupt(u32 flag) {
  if (auto* exi = hw_.device<hw::ExiBus>()) exi->baseboard_interrupt(flag);
}

bool MediaBoard::execute(std::array<u32, 3>& command, u32& immediate, u32 address, u32 length) {
  // Commands are obfuscated with a key that evolves with each command.
  command[0] ^= key_a_;
  command[1] ^= key_b_;
  command[2] ^= key_c_;
  const u32 seed = command[0] >> 16;
  key_a_ *= seed;
  key_b_ *= seed;
  key_c_ *= seed;
  // The keys for the next command are in low memory. The driver sends two
  // dummy commands first, so starting from there loses nothing.
  if (key_a_ == 0) {
    if (const u8* low = guest(0x80000000, 12); low && get_be32(low)) {
      key_a_ = get_be32(low);
      key_b_ = get_be32(low + 4);
      key_c_ = get_be32(low + 8);
      mb_log("keys %08X %08X %08X", key_a_, key_b_, key_c_);
    }
  }

  const u8 di_command = static_cast<u8>(command[0]);
  const u32 offset = command[1] << 2;
  mb_log("command %02X offset %08X addr %08X len %08X", di_command, offset, address, length);

  switch (di_command) {
    case DI_INQUIRY:
      immediate = game_ == Game::Other ? INQUIRY_VERSION1 : INQUIRY_VERSION2;
      return true;
    case DI_READ:
      if (read(offset, address, length)) return true;
      // Let the drive report the failure (with the command byte where it expects it).
      command[0] <<= 24;
      return false;
    case DI_WRITE: write(offset, address, length); return true;
    case DI_EXECUTE:
      if (offset == 0 && length == 0) execute_board_command();
      else if (mb16_at(0x22) != CMD_UNKNOWN_000) mb_error("unhandled execute %04X", mb16_at(0x22));
      return true;
    default: mb_error("unhandled command %02X", di_command); return true;
  }
}

bool MediaBoard::read(u32 offset, u32 address, u32 length) {
  if ((offset & 0x8FFF0000) == 0x80000000) {
    u8* dst = guest(address, std::max<u32>(length, 8));
    if (!dst) return true;
    switch (offset) {
      case MB_STATUS1: dst[0] = 0, dst[1] = 1; break;
      case MB_STATUS2: std::memset(dst, 0, length); break;
      case MB_STATUS3:
        std::memset(dst, 0xFF, length);
        put_le32(dst, 0x20000000);      // DIMM size (512 MiB)
        put_be32(dst + 4, 0x4743414D);  // "GCAM"
        break;
      case 0x80000100: put_le32(dst, 0x001F1F1F); break;
      case FIRMWARE_STATUS1: put_le32(dst, 0x01FA); break;
      case FIRMWARE_STATUS2: put_le32(dst, 1); break;
      case 0x80000160: put_be32(dst, 0x00001E00); break;
      case 0x80000180: put_be32(dst, 0); break;
      case 0x800001A0: put_be32(dst, 0xFFFFFFFF); break;
      default: mb_error("unhandled status read %08X length %08X", offset, length); break;
    }
    return true;
  }

  auto file_read = [&](File& file, u32 file_offset) {
    if (u8* dst = guest(address, length)) file.read(file_offset, dst, length);
    return true;
  };
  if (offset == 0 && length == 0x80) return file_read(netcfg_, 0);  // network configuration
  if (offset == DIMM_EXTRA_SETTINGS && length == 0x20) return file_read(extra_, 0);  // media CRC check
  if (offset >= DIMM_MEMORY && offset < 0x1F800000) return file_read(dimm_, offset - DIMM_MEMORY);
  if (offset >= DIMM_MEMORY2 && offset < 0xFF800000) return file_read(dimm_, offset - DIMM_MEMORY2);
  if (offset == NETWORK_CONTROL && length == 0x20) return file_read(netctrl_, 0);

  if (offset >= ALLNET_BUFFER && offset < ALLNET_BUFFER + 0x1000) {
    const u32 skip = offset - ALLNET_BUFFER;
    if (u8* dst = guest(address, sizeof(kAllNetReply)); dst && skip < sizeof(kAllNetReply))
      std::memcpy(dst, kAllNetReply + skip, sizeof(kAllNetReply) - skip);
    return true;
  }

  // The 128-byte read after GetNetworkConfig returns the network configuration.
  if (netconfig_read_pending_ && length == 0x80 && !span_for(offset).empty()) {
    netconfig_read_pending_ = false;
    u8 config[0x80];
    netcfg_.read(0, config, sizeof(config));
    if (config[0] == 0) config[0] = 2;  // a menu table index; entry 0 is invalid
    if (u8* dst = guest(address, sizeof(config))) std::memcpy(dst, config, sizeof(config));
    return true;
  }

  // Both Execute paths answer through the media buffer; each keeps its own copy.
  if (offset == DIMM_COMMAND_VERSION2 && length == 0x20) {
    if (u8* dst = guest(address, 0x20)) std::memcpy(dst, exec1_response_.data(), 0x20);
    return true;
  }
  if (offset == DIMM_COMMAND_VERSION2_2 && length == 0x20) {
    if (u8* dst = guest(address, 0x20)) std::memcpy(dst, exec2_response_.data(), 0x20);
    return true;
  }

  if (const auto span = span_for(offset); !span.empty()) {
    if (u8* dst = guest(address, length); dst && length <= span.size()) std::memcpy(dst, span.data(), length);
    return true;
  }

  if (offset == DIMM_COMMAND_EXECUTE2) {
    execute2(address, length);
    return true;
  }

  if (offset >= MAX_DISC_OFFSET) {
    mb_error("unhandled read %08X length %08X", offset, length);
    return true;
  }

  // The game image. The DIMM holds it in more memory than it needs, so a read
  // that runs past its end gets zeros there instead of failing. The games
  // read whole 32-byte blocks: GP 2's last file, Bowser's fire, ends 2 bytes
  // before the image does, and loading it reads 20 bytes past the end. A
  // failed read would hang the game: the DVD library on the Triforce never
  // asks the drive what went wrong (its DVDLowRequestError does nothing).
  u8* dst = guest(address, length);
  if (!dst) return false;
  const u32 n = offset < disc_.size() ? static_cast<u32>(std::min<u64>(length, disc_.size() - offset)) : 0;
  std::memset(dst + n, 0, length - n);
  if (n && !disc_.read(offset, dst, n)) {
    mb_error("cannot read the game image at %08X (length %08X)", offset, n);
    return false;
  }
  return true;
}

void MediaBoard::write(u32 offset, u32 address, u32 length) {
  const u8* src = guest(address, length);
  if (!src) return;

  if ((offset == FIRMWARE_MAGIC_WRITE1 || offset == FIRMWARE_MAGIC_WRITE2) && length == 0x20) {
    mb_error("firmware update requested (not supported)");
    return;
  }
  if (offset == 0 && length == 0x80) return netcfg_.write(0, src, length);
  if (offset == DIMM_EXTRA_SETTINGS && length == 0x20) return extra_.write(0, src, length);
  if (offset >= BACKUP_MEMORY && offset < 0x00800000) return backup_.write(0, src, length);
  if (offset >= DIMM_MEMORY && offset < 0x1F800000) return dimm_.write(offset - DIMM_MEMORY, src, length);
  if (offset >= FIRMWARE_ADDRESS && offset < 0x84818000) return;
  if (offset >= DIMM_MEMORY2 && offset < 0xFF800000) return dimm_.write(offset - DIMM_MEMORY2, src, length);
  if (offset == NETWORK_CONTROL && length == 0x20) return netctrl_.write(0, src, length);

  if (offset == DIMM_COMMAND_EXECUTE1 && length == 0x20 && src[0] == 1) return execute1();

  if (const auto span = span_for(offset); !span.empty()) {
    // SET IP ADDRESS: the configuration arrives before its command.
    if (length == 0x80 && src[0] != 0) netcfg_.write(0, src, length);
    if (length <= span.size()) std::memcpy(span.data(), src, length);
    return;
  }

  if (offset >= MAX_DISC_OFFSET) mb_error("unhandled write %08X length %08X", offset, length);
}

void MediaBoard::execute_board_command() {
  const u16 command = mb16_at(0x22);
  if (command == CMD_UNKNOWN_000) return;
  std::memset(media_buffer_.data(), 0, 0x20);
  set_mb32(0, mb32(8) | 0x80000000);  // counter/command, done
  mb_log("execute %04X", command);

  switch (command) {
    case CMD_GET_DIMM_SIZE: set_mb32(1, 0x20000000); break;
    case CMD_GET_MEDIA_BOARD_STATUS:
      // Pretend to still be loading for a moment (a chance to enter test mode).
      set_mb32(1, board_status_);
      set_mb32(2, load_progress_);
      if (load_progress_ < 100) ++load_progress_;
      else board_status_ = STATUS_LOADED_GAME_PROGRAM;
      break;
    case CMD_GET_SEGABOOT_VERSION:  // 3.11
      media_buffer_[4] = 0x03;
      media_buffer_[5] = 0x11;
      media_buffer_[6] = 1;
      set_mb32(2, 1);
      set_mb32(4, 0xFF);
      break;
    case CMD_GET_SYSTEM_FLAGS:
      media_buffer_[4] = 1;
      media_buffer_[5] = MEDIA_GDROM;
      media_buffer_[6] = 1;  // development mode (also allows region-free booting)
      media_buffer_[8] = 0;  // access count
      break;
    case CMD_GET_MEDIA_BOARD_SERIAL: std::memcpy(&media_buffer_[4], "A89E-27A50364511", 16); break;
    case CMD_GET_NETWORK_CONFIG: media_buffer_[4] = 1; break;
    case CMD_NETWORK_REINIT: break;
    case CMD_TEST_HARDWARE:
      // Test type in [9]; [12] points at a string the game displays.
      if (u8* p = guest(mb32(12), 8)) put_le32(p, TEST_OK_WORD0), put_le32(p + 4, TEST_OK_WORD1);
      set_mb32(1, mb32(9));
      break;
    case CMD_INIT_LINK: break;
    default:
      if (command >= NET_ACCEPT && command <= NET_MODIFY_MY_IP_ADDR) socket_command(command, 10);
      else mb_error("unhandled execute command %04X", command);
      break;
  }

  std::memset(&media_buffer_[0x20], 0, 0x20);
  std::memcpy(exec1_response_.data(), media_buffer_.data(), 0x20);
}

void MediaBoard::execute1() {
  const u16 command = mb16_at(0x22);
  mb_log("execute1 %04X", command);
  std::memset(media_buffer_.data(), 0, 0x20);
  set_mb32(0, mb32(8) | 0x80000000);

  switch (command) {
    case CMD_UNKNOWN_000: set_mb32(1, 1); break;
    case CMD_GET_DIMM_SIZE: set_mb32(1, 0x20000000); break;
    case CMD_GET_MEDIA_BOARD_STATUS:
      set_mb32(1, STATUS_LOADED_GAME_PROGRAM);
      set_mb32(2, 100);
      break;
    case CMD_GET_SEGABOOT_VERSION:  // 3.09
      media_buffer_[4] = 0x03;
      media_buffer_[5] = 0x09;
      media_buffer_[6] = 1;
      set_mb32(2, 1);
      set_mb32(4, 0xFF);
      break;
    case CMD_GET_SYSTEM_FLAGS:
      media_buffer_[4] = 1;
      media_buffer_[5] = MEDIA_GDROM;
      media_buffer_[6] = 1;
      media_buffer_[8] = 0;
      break;
    case CMD_GET_MEDIA_BOARD_SERIAL: std::memcpy(&media_buffer_[4], "A89E-27A50364511", 16); break;
    case CMD_GET_NETWORK_CONFIG: media_buffer_[4] = 1; break;
    case CMD_TEST_HARDWARE: {
      // [9] = test type, [10] = string pointer.
      if (u8* p = guest(mb32(10), 8); p && mb32(10)) put_le32(p, TEST_OK_WORD0), put_le32(p + 4, TEST_OK_WORD1);
      set_mb32(1, mb32(9));
      schedule_test_result(false);
      break;
    }
    default:
      // 0x8xxx: cleanup acknowledgements.
      if (!(command & 0x8000)) mb_error("unhandled execute1 command %04X", command);
      break;
  }

  std::memset(&media_buffer_[0x20], 0, 0x20);
  std::memcpy(exec1_response_.data(), media_buffer_.data(), 0x20);
  raise_interrupt(0x04);
}

void MediaBoard::execute2(u32 address, u32 length) {
  const u16 command = mb16_at(0x202);
  mb_log("execute2 %04X", command);
  std::memcpy(media_buffer_.data(), &media_buffer_[0x200], 0x20);
  std::memset(&media_buffer_[0x200], 0, 0x20);
  media_buffer_[0x204] = 1;

  switch (command) {
    case CMD_GET_DIMM_SIZE: set_mb32(1, 1); break;  // Unknown_001 on the network board
    case CMD_GET_SEGABOOT_VERSION:  // network firmware 13.05, VxWorks
      set_mb32(1, 0x1305);
      media_buffer_[6] = 1;
      break;
    case CMD_GET_SYSTEM_FLAGS:
      media_buffer_[4] = 1;
      media_buffer_[6] = BOARD_NAND_MASK_NAND;
      media_buffer_[7] = 1;
      break;
    case CMD_GET_MEDIA_BOARD_SERIAL: break;  // Unknown_103: empty reply
    case CMD_GET_NETWORK_CONFIG:
      media_buffer_[4] = 1;
      netconfig_read_pending_ = true;  // the game reads the configuration next
      break;
    case CMD_NETWORK_REINIT: break;
    case CMD_TEST_HARDWARE: {
      // [1] = test type, [2] = string pointer.
      if (u8* p = guest(mb32(2), 8); p && mb32(2)) put_le32(p, TEST_OK_WORD0), put_le32(p + 4, TEST_OK_WORD1);
      schedule_test_result(true);
      break;
    }
    case CMD_INIT_LINK:
    case CMD_ALLNET_INIT: break;
    default:
      if (command >= NET_ACCEPT && command <= NET_MODIFY_MY_IP_ADDR) socket_command(command, 2);
      else if (!(command & 0x8000)) mb_error("unhandled execute2 command %04X", command);
      break;
  }

  media_buffer_[3] |= 0x80;  // done
  std::memcpy(exec2_response_.data(), media_buffer_.data(), 0x20);
  if (u8* dst = guest(address, length)) std::memset(dst, 0, length);
  raise_interrupt(0x10);
}

// The parameters are the command block's words from `parameter_index` on;
// the result is the reply's second word. Most replies also echo byte 8 of the
// block in byte 1 (a sequence number), as Dolphin's do.
void MediaBoard::socket_command(u16 command, u32 parameter_index) {
  std::array<u32, 6> params;
  for (u32 i = 0; i < params.size(); ++i) params[i] = mb32(parameter_index + i);
  const u32 result = network_.execute(command, params);
  switch (command) {
    case NET_SOCKET:
    case NET_SELECT:
    case NET_GET_PARAM_BY_DHCP_EXEC: media_buffer_[1] = 0; break;
    case NET_CLOSESOCKET:
    case NET_ACCEPT:
    case NET_BIND:
    case NET_MODIFY_MY_IP_ADDR: break;
    default: media_buffer_[1] = media_buffer_[8]; break;
  }
  if (command != NET_MODIFY_MY_IP_ADDR) set_mb32(1, result);
}

void MediaBoard::schedule_test_result(bool execute2) {
  if (test_event_) hw_.cancel(test_event_);
  test_event_ = hw_.schedule_after(hw::kTimebaseHz / 10000, [this, execute2] {
    test_event_ = 0;
    auto& response = execute2 ? exec2_response_ : exec1_response_;
    response.fill(0);
    put_le32(&response[0], 0x03020000);  // sub-command 2, class 3
    put_le32(&response[4], 2);           // test status: good
    put_le32(&response[8], 100);         // progress
    raise_interrupt(execute2 ? 0x10 : 0x04);
  });
}

}  // namespace triforce
