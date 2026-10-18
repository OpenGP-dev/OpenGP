// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's EXI_Channel.cpp, EXI_DeviceIPL.cpp and
// EXI_DeviceBaseboard.cpp (Copyright Dolphin Emulator Project, GPL-2.0-or-later).

#include "hw/exi.h"
#include "os/notice.h"
#include "os/utf8.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

// Replacement boot ROM fonts (generated from thirdparty/gc-fonts).
extern const unsigned char kIplFont_western[], kIplFont_japanese[];
extern const std::size_t kIplFont_western_size, kIplFont_japanese_size;

namespace hw {

namespace {

// Channel status register bits.
constexpr u32 ST_EXIINTMASK = 0x0001;
constexpr u32 ST_EXIINT = 0x0002;
constexpr u32 ST_TCINTMASK = 0x0004;
constexpr u32 ST_TCINT = 0x0008;
constexpr u32 ST_CLK = 0x0070;
constexpr u32 ST_CS = 0x0380;  // one-hot chip select (1, 2, 4) at bit 7
constexpr u32 ST_EXTINTMASK = 0x0400;
constexpr u32 ST_EXTINT = 0x0800;
constexpr u32 ST_EXT = 0x1000;
constexpr u32 ST_ROMDIS = 0x2000;

// Control register bits.
constexpr u32 CR_TSTART = 0x1;
constexpr u32 CR_DMA = 0x2;

enum : u32 { EXI_READ = 0, EXI_WRITE = 1, EXI_READWRITE = 2 };

// IPL chip address map (command bits 6-30).
constexpr u32 ROM_SIZE = 0x200000;
constexpr u32 SRAM_BASE = 0x800000;
constexpr u32 SRAM_SIZE = 0x44;
constexpr u32 UART_BASE = 0x800400;
constexpr u32 UART_SIZE = 0x50;

// Baseboard commands.
enum : u8 {
  BB_BACKUP_OFFSET_SET = 0x01,
  BB_BACKUP_WRITE = 0x02,
  BB_BACKUP_READ = 0x03,
  BB_DMA_OFFSET_LENGTH_SET = 0x05,
  BB_READ_ISR = 0x82,
  BB_WRITE_ISR = 0x83,
  BB_READ_IMR = 0x86,
  BB_WRITE_IMR = 0x87,
  BB_WRITE_LANCNT = 0xFF,
};

u32 rtc_seconds_since_2000() {
  const std::time_t now = std::time(nullptr);
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &now);
  gmtime_s(&utc, &now);
#else
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
#endif
  local.tm_isdst = 0;
  utc.tm_isdst = 0;
  const auto offset = static_cast<long long>(std::difftime(std::mktime(&local), std::mktime(&utc)));
  return static_cast<u32>(static_cast<long long>(now) + offset - 946684800LL);
}

}  // namespace

// ---- ExiDevice defaults (byte-by-byte) ------------------------------------------------------

void ExiDevice::imm_write(u32 data, u32 size) {
  while (size--) {
    u8 byte = static_cast<u8>(data >> 24);
    transfer_byte(byte);
    data <<= 8;
  }
}

u32 ExiDevice::imm_read(u32 size) {
  u32 result = 0;
  for (u32 i = 0; i < size; ++i) {
    u8 byte = 0;
    transfer_byte(byte);
    result |= u32{byte} << (24 - 8 * i);
  }
  return result;
}

void ExiDevice::dma_write(u8* src, u32 size) {
  for (u32 i = 0; i < size; ++i) {
    u8 byte = src[i];
    transfer_byte(byte);
  }
}

void ExiDevice::dma_read(u8* dst, u32 size) {
  for (u32 i = 0; i < size; ++i) {
    u8 byte = 0;
    transfer_byte(byte);
    dst[i] = byte;
  }
}

// ---- IPL chip --------------------------------------------------------------------------------

IplDevice::IplDevice(const std::string& user_dir) : rom_(ROM_SIZE, 0) {
  // Fonts: Shift JIS at 0x1AFF00, Windows-1252 at 0x1FCF00 (Yay0-compressed).
  constexpr u32 kSjisOffset = 0x1AFF00, kSjisSize = 0x4A24D;
  constexpr u32 kAnsiOffset = 0x1FCF00, kAnsiSize = 0x2575;
  bool loaded = false;
  if (std::FILE* f = os::fopen_utf8(user_dir + "/IPL.bin", "rb")) {
    loaded = std::fseek(f, kSjisOffset, SEEK_SET) == 0 && std::fread(&rom_[kSjisOffset], 1, kSjisSize, f) == kSjisSize &&
             std::fseek(f, kAnsiOffset, SEEK_SET) == 0 && std::fread(&rom_[kAnsiOffset], 1, kAnsiSize, f) == kAnsiSize;
    std::fclose(f);
    if (!loaded) std::fprintf(stderr, "[exi] %s/IPL.bin is too short; using the replacement fonts\n", user_dir.c_str());
  }
  if (!loaded) {
    std::memcpy(&rom_[kSjisOffset], kIplFont_japanese, std::min<std::size_t>(kIplFont_japanese_size, ROM_SIZE - kSjisOffset));
    std::memcpy(&rom_[kAnsiOffset], kIplFont_western, std::min<std::size_t>(kIplFont_western_size, ROM_SIZE - kAnsiOffset));
  }

  // Default SRAM settings (after the 4-byte RTC): checksum, ~checksum, ead0,
  // ead1, rtc_bias, horizontal offset, ntd, language, flags.
  u8* s = sram_.data() + 4;
  s[0x13] = 0x2C;  // flags, as in Dolphin's default SRAM
  // The checksums cover the four 16-bit words from rtc_bias (0x0C) to 0x14.
  u16 sum = 0, inv = 0;
  for (u32 i = 0x0C; i < 0x14; i += 2) {
    const u16 v = static_cast<u16>((s[i] << 8) | s[i + 1]);
    sum = static_cast<u16>(sum + v);
    inv = static_cast<u16>(inv + static_cast<u16>(~v));
  }
  s[0] = static_cast<u8>(sum >> 8);
  s[1] = static_cast<u8>(sum);
  s[2] = static_cast<u8>(inv >> 8);
  s[3] = static_cast<u8>(inv);
}

void IplDevice::set_cs(bool selected) {
  if (selected) {
    command_bytes_ = 0;
    cursor_ = 0;
  }
}

void IplDevice::transfer_byte(u8& data) {
  if (command_bytes_ < 4) {
    command_ = (command_ << 8) | data;
    data = 0xFF;
    if (++command_bytes_ == 4) {
      const u32 rtc = rtc_seconds_since_2000();
      sram_[0] = static_cast<u8>(rtc >> 24);
      sram_[1] = static_cast<u8>(rtc >> 16);
      sram_[2] = static_cast<u8>(rtc >> 8);
      sram_[3] = static_cast<u8>(rtc);
    }
    return;
  }
  const bool is_write = (command_ >> 31) & 1;
  const u32 address = (command_ >> 6) & 0x1FFFFFF;
  if (address < ROM_SIZE) {
    // The boot ROM (only its fonts are provided).
    if (!is_write) data = rom_[(address + cursor_) % ROM_SIZE];
    ++cursor_;
  } else if (address >= SRAM_BASE && address < SRAM_BASE + SRAM_SIZE) {
    const u32 i = (address - SRAM_BASE + cursor_++) % SRAM_SIZE;
    if (is_write) sram_[i] = data;
    else data = sram_[i];
  } else if (address >= UART_BASE && address < UART_BASE + UART_SIZE) {
    if (address - UART_BASE == 0) {
      if (is_write) {
        if (data == '\r' || data == '\n') {
          if (!uart_line_.empty()) std::fprintf(stdout, "[UART] %s\n", uart_line_.c_str());
          uart_line_.clear();
        } else if (data) {
          uart_line_ += static_cast<char>(data);
        }
      } else {
        data = 0;  // queue length: we are instant
      }
    }
  }
}

// ---- Triforce baseboard ----------------------------------------------------------------------

BaseboardDevice::BaseboardDevice(std::string backup_path) : backup_path_(std::move(backup_path)) {
  backup_ = os::fopen_utf8(backup_path_, "rb+");
  if (!backup_) backup_ = os::fopen_utf8(backup_path_, "wb+");
  if (!backup_) backup_failed();
}

// The game's settings and records are not being kept: say so once.
void BaseboardDevice::backup_failed() {
  if (backup_warned_) return;
  backup_warned_ = true;
  os::notify_player("The game's settings and records cannot be saved to " + backup_path_ +
                    ": check that the user folder can be written to.");
}

BaseboardDevice::~BaseboardDevice() {
  if (backup_) std::fclose(backup_);
}

void BaseboardDevice::set_cs(bool selected) {
  if (selected) position_ = 0;
}

bool BaseboardDevice::interrupt() {
  if (!interrupt_set_) return false;
  if (++irq_timer_ > 12) interrupt_set_ = false;
  return true;
}

void BaseboardDevice::dma_write(u8* src, u32 size) {
  if (!backup_) return;
  if (std::fseek(backup_, static_cast<long>(backup_offset_), SEEK_SET) != 0 ||
      std::fwrite(src, 1, size, backup_) != size || std::fflush(backup_) != 0)
    backup_failed();
}

void BaseboardDevice::dma_read(u8* dst, u32 size) {
  std::memset(dst, 0, size);
  if (!backup_) return;
  std::fseek(backup_, static_cast<long>(backup_offset_), SEEK_SET);
  (void)std::fread(dst, 1, size, backup_);
}

void BaseboardDevice::transfer_byte(u8& byte) {
  if (position_ < 4) {
    command_[position_] = byte;
    byte = 0xFF;
  }
  if (position_ >= 2 && command_[0] == 0 && command_[1] == 0) {
    byte = static_cast<u8>("\x06\x04\x10\x00"[(position_ - 2) & 3]);  // serial ID
  } else if (position_ == 4) {
    switch (command_[0]) {
      case BB_BACKUP_OFFSET_SET:
        backup_offset_ = (u32{command_[1]} << 8) | command_[2];
        byte = 0x01;
        break;
      case BB_BACKUP_WRITE:
        if (backup_ && (std::fseek(backup_, static_cast<long>(backup_offset_), SEEK_SET) != 0 ||
                        std::fwrite(&command_[1], 1, 1, backup_) != 1 || std::fflush(backup_) != 0))
          backup_failed();
        byte = 0x01;
        break;
      case BB_BACKUP_READ:
      case BB_DMA_OFFSET_LENGTH_SET: byte = 0x01; break;
      case BB_READ_ISR:
      case BB_READ_IMR:
      case BB_WRITE_IMR: byte = 0x04; break;
      case BB_WRITE_ISR:
        irq_status_ &= ~u32{command_[2]};
        byte = 0x04;
        break;
      case BB_WRITE_LANCNT:
        if (command_[1] == 0 && command_[2] == 0) {
          interrupt_set_ = true;
          irq_timer_ = 0;
          irq_status_ = 0x02;
        }
        if (command_[1] == 2 && command_[2] == 1) irq_status_ = 0;
        byte = 0x08;
        break;
      default: byte = 0x04; break;
    }
  } else if (position_ > 4) {
    switch (command_[0]) {
      case BB_BACKUP_READ:
        if (backup_) {
          std::fseek(backup_, static_cast<long>(backup_offset_), SEEK_SET);
          if (std::fread(&byte, 1, 1, backup_) != 1) byte = 0;
        }
        break;
      case BB_DMA_OFFSET_LENGTH_SET: byte = 0x01; break;
      case BB_READ_ISR:
        if (position_ == 6) {
          byte = static_cast<u8>(irq_status_);
          interrupt_set_ = false;
        } else {
          byte = 0x04;
        }
        break;
      case BB_READ_IMR:
        if (position_ == 5) byte = 0xFF;
        if (position_ == 6) byte = 0x81;
        break;
      default: break;
    }
  } else if (position_ < 4 && !(position_ >= 2 && command_[0] == 0 && command_[1] == 0)) {
    byte = 0xFF;
  }
  ++position_;
}

// ---- EXI bus ---------------------------------------------------------------------------------

ExiDevice* ExiBus::Channel::selected() const {
  switch ((status & ST_CS) >> 7) {
    case 1: return devices[0].get();
    case 2: return devices[1].get();
    case 4: return devices[2].get();
    default: return nullptr;
  }
}

ExiBus::ExiBus(Hardware& hw, const std::string& user_dir, const std::string& game_id) : Device(hw) {
  if (const char* env = std::getenv("OPENGP_LOG_EXI")) log_remaining_ = std::atoi(env) > 1 ? std::atoi(env) : 200;
  std::error_code ec;
  std::filesystem::create_directories(user_dir, ec);
  channels_[0].status = ST_EXTINT;
  channels_[1].status = ST_EXTINT | (1u << 7);
  channels_[0].devices[1] = std::make_unique<IplDevice>(user_dir);
  channels_[0].devices[2] = std::make_unique<BaseboardDevice>(user_dir + "/tribackup_" + game_id + ".bin");
}

u32 ExiBus::read(u32 offset, u32 size) {
  const u32 index = offset / 0x14;
  if (index >= 3 || size != 4) return 0;
  Channel& c = channels_[index];
  switch (offset % 0x14) {
    case 0x00: {
      // EXT: a device is connected in the memory card position (CS 1).
      c.status &= ~ST_EXT;
      if (index != 2 && c.devices[0] && c.devices[0]->present()) c.status |= ST_EXT;
      return c.status;
    }
    case 0x04: return c.dma_address;
    case 0x08: return c.dma_length;
    case 0x0C: return c.control;
    case 0x10: return c.imm_data;
    default: return 0;
  }
}

void ExiBus::write(u32 offset, u32 v, u32 size) {
  const u32 index = offset / 0x14;
  if (index >= 3 || size != 4) return;
  Channel& c = channels_[index];
  switch (offset % 0x14) {
    case 0x00: {
      c.status = (c.status & ~(ST_EXIINTMASK | ST_TCINTMASK | ST_CLK)) | (v & (ST_EXIINTMASK | ST_TCINTMASK | ST_CLK));
      if (v & ST_EXIINT) c.status &= ~ST_EXIINT;
      if (v & ST_TCINT) c.status &= ~ST_TCINT;
      if (index != 2) {
        c.status = (c.status & ~ST_EXTINTMASK) | (v & ST_EXTINTMASK);
        if (v & ST_EXTINT) c.status &= ~ST_EXTINT;
      }
      if (index == 0) c.status = (c.status & ~ST_ROMDIS) | (v & ST_ROMDIS);
      const u32 old_cs = (c.status & ST_CS) >> 7;
      const u32 new_cs = (v & ST_CS) >> 7;
      c.status = (c.status & ~ST_CS) | (v & ST_CS);
      // The device whose select line changed hears about it.
      const u32 changed = old_cs ^ new_cs;
      ExiDevice* dev = changed == 1 ? c.devices[0].get() : changed == 2 ? c.devices[1].get()
                     : changed == 4 ? c.devices[2].get() : nullptr;
      if (dev) dev->set_cs(new_cs != 0);
      update_interrupts();
      break;
    }
    case 0x04: c.dma_address = v; break;
    case 0x08: c.dma_length = v; break;
    case 0x0C: {
      c.control = v;
      if (!(v & CR_TSTART)) break;
      if (log_remaining_ > 0) {
        --log_remaining_;
        std::fprintf(stderr, "[exi] ch%u cs%u %s %s len %u data %08X addr %08X\n", index, (c.status & ST_CS) >> 7,
                     (v & CR_DMA) ? "dma" : "imm", ((v >> 2) & 3) == EXI_READ ? "read" : ((v >> 2) & 3) == EXI_WRITE ? "write" : "rw",
                     (v & CR_DMA) ? c.dma_length : ((v >> 4) & 3) + 1, c.imm_data, c.dma_address);
      }
      if (ExiDevice* dev = c.selected()) {
        const u32 rw = (v >> 2) & 3;
        if (!(v & CR_DMA)) {
          const u32 len = ((v >> 4) & 3) + 1;
          if (rw == EXI_READ) c.imm_data = dev->imm_read(len);
          else if (rw == EXI_WRITE) dev->imm_write(c.imm_data, len);
        } else {
          u8* mem = hw_.ram(c.dma_address & ~31u, c.dma_length);
          if (mem) {
            if (rw == EXI_READ) dev->dma_read(mem, c.dma_length);
            else if (rw == EXI_WRITE) dev->dma_write(mem, c.dma_length);
          }
        }
      }
      c.control &= ~CR_TSTART;
      c.status |= ST_TCINT;
      update_interrupts();
      break;
    }
    case 0x10: c.imm_data = v; break;
    default: break;
  }
}

void ExiBus::baseboard_interrupt(u32 flag) {
  if (auto* bb = dynamic_cast<BaseboardDevice*>(channels_[0].devices[2].get())) bb->generate_interrupt(flag);
  update_interrupts();
}

bool ExiBus::causing_interrupt(int index) {
  Channel& c = channels_[index];
  if (index == 2) {
    // The baseboard's interrupt line shows up on channel 2.
    ExiDevice* bb = channels_[0].devices[2].get();
    if (bb && bb->interrupt()) c.status |= ST_EXIINT;
  } else if (ExiDevice* dev = c.selected(); dev && dev->interrupt()) {
    c.status |= ST_EXIINT;
  }
  return ((c.status & ST_EXIINT) && (c.status & ST_EXIINTMASK)) ||
         ((c.status & ST_TCINT) && (c.status & ST_TCINTMASK)) ||
         ((c.status & ST_EXTINT) && (c.status & ST_EXTINTMASK));
}

void ExiBus::update_interrupts() {
  bool active = false;
  for (int i = 0; i < 3; ++i) active |= causing_interrupt(i);
  hw_.set_interrupt(INT_EXI, active);
}

}  // namespace hw
