// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's DVDInterface.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).

#include "hw/di.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hw {

namespace {

// Register offsets.
enum : u32 {
  DI_SR = 0x00,
  DI_CVR = 0x04,
  DI_CMD0 = 0x08,
  DI_CMD1 = 0x0C,
  DI_CMD2 = 0x10,
  DI_MAR = 0x14,
  DI_LENGTH = 0x18,
  DI_CR = 0x1C,
  DI_IMM = 0x20,
  DI_CFG = 0x24,
};

// Status register.
constexpr u32 SR_BREAK = 0x01;
constexpr u32 SR_DEINTMASK = 0x02;
constexpr u32 SR_DEINT = 0x04;
constexpr u32 SR_TCINTMASK = 0x08;
constexpr u32 SR_TCINT = 0x10;
constexpr u32 SR_BRKINTMASK = 0x20;
constexpr u32 SR_BRKINT = 0x40;

// Cover register.
constexpr u32 CVR_OPEN = 0x1;
constexpr u32 CVR_INTMASK = 0x2;
constexpr u32 CVR_INT = 0x4;

// Control register.
constexpr u32 CR_TSTART = 0x1;

// Drive error codes (RequestError).
constexpr u32 ERROR_INVALID_COMMAND = 0x052000;
constexpr u32 ERROR_BLOCK_OOB = 0x056300;

}  // namespace

DiDevice::DiDevice(Hardware& hw, const disc::Image& disc, bool triforce) : Device(hw), disc_(disc) {
  if (const char* env = std::getenv("OPENGP_LOG_DI")) log_remaining_ = std::atoi(env) > 1 ? std::atoi(env) : 200;
  cfg_ = 1;  // boot ROM descrambler disabled
  if (triforce) {
    cvr_ = 0;   // the Triforce IPL expects the cover closed
    cfg_ |= 8;  // Triforce memory configuration
  } else {
    cvr_ = disc.is_open() ? 0 : CVR_OPEN;
  }
}

u32 DiDevice::read(u32 offset, u32 size) {
  if (size != 4) return 0;
  switch (offset) {
    case DI_SR: return sr_;
    case DI_CVR: return cvr_;
    case DI_CMD0: return cmd_[0];
    case DI_CMD1: return cmd_[1];
    case DI_CMD2: return cmd_[2];
    case DI_MAR: return mar_;
    case DI_LENGTH: return length_;
    case DI_CR: return cr_;
    case DI_IMM: return imm_;
    case DI_CFG: return cfg_;
    default: return 0;
  }
}

void DiDevice::write(u32 offset, u32 v, u32 size) {
  if (size != 4) return;
  switch (offset) {
    case DI_SR:
      sr_ = (sr_ & ~(SR_DEINTMASK | SR_TCINTMASK | SR_BRKINTMASK | SR_BREAK)) |
            (v & (SR_DEINTMASK | SR_TCINTMASK | SR_BRKINTMASK | SR_BREAK));
      sr_ &= ~(v & (SR_DEINT | SR_TCINT | SR_BRKINT));  // write 1 to clear
      update_interrupts();
      break;
    case DI_CVR:
      cvr_ = (cvr_ & ~CVR_INTMASK) | (v & CVR_INTMASK);
      if (v & CVR_INT) cvr_ &= ~CVR_INT;
      update_interrupts();
      break;
    case DI_CMD0: cmd_[0] = v; break;
    case DI_CMD1: cmd_[1] = v; break;
    case DI_CMD2: cmd_[2] = v; break;
    case DI_MAR: mar_ = v & ~0xFC00001Fu; break;
    case DI_LENGTH: length_ = v & ~0x1Fu; break;
    case DI_CR:
      cr_ = v & 7;
      if (cr_ & CR_TSTART) execute();
      break;
    case DI_IMM: imm_ = v; break;
    default: break;
  }
}

void DiDevice::execute() {
  if (log_remaining_ > 0) {
    --log_remaining_;
    std::fprintf(stderr, "[di] command %08X %08X %08X addr %08X len %08X\n", cmd_[0], cmd_[1], cmd_[2], mar_, length_);
  }
  bool ok = true;
  if (!backend_ || !backend_->execute(cmd_, imm_, mar_, length_)) ok = drive_command();

  // Commands complete immediately.
  cr_ &= ~CR_TSTART;
  if (ok) {
    mar_ += length_;
    length_ = 0;
    sr_ |= SR_TCINT;
  } else {
    // Said out loud: on the Triforce, a failed command hangs the game.
    std::fprintf(stderr, "[di] command %08X %08X %08X failed (address %08X, length %08X)\n", cmd_[0], cmd_[1],
                 cmd_[2], mar_, length_);
    sr_ |= SR_DEINT;
  }
  update_interrupts();
}

bool DiDevice::drive_command() {
  const u8 command = static_cast<u8>(cmd_[0] >> 24);
  if (command != 0xE0) error_ = 0;
  switch (command) {
    case 0x12: {  // inquiry
      static constexpr u8 kInquiry[12] = {0x00, 0x00, 0x00, 0x02, 0x20, 0x06, 0x05, 0x26, 0x41, 0x00, 0x00, 0x00};
      if (u8* dst = hw_.ram(mar_, sizeof(kInquiry))) std::memcpy(dst, kInquiry, sizeof(kInquiry));
      return true;
    }
    case 0xA8: {  // read (0x40 in the low byte: disc ID)
      const u64 offset = (cmd_[0] & 0xC0) == 0x40 ? 0 : u64{cmd_[1]} << 2;
      u8* dst = hw_.ram(mar_, length_);
      if (!dst || offset + length_ > disc_.size()) {
        error_ = ERROR_BLOCK_OOB;
        return false;
      }
      disc_.read(offset, dst, length_);
      return true;
    }
    case 0xAB:  // seek
    case 0xE3:  // stop motor
    case 0xE4:  // audio buffer configuration
      return true;
    case 0xE0:  // request error
      imm_ = error_;
      return true;
    case 0xE1:  // audio stream
    case 0xE2:  // audio stream status
      imm_ = 0;
      return true;
    default:
      std::fprintf(stderr, "[di] unknown drive command %08X\n", cmd_[0]);
      error_ = ERROR_INVALID_COMMAND;
      return false;
  }
}

void DiDevice::update_interrupts() {
  const bool asserted = ((sr_ & SR_DEINT) && (sr_ & SR_DEINTMASK)) || ((sr_ & SR_TCINT) && (sr_ & SR_TCINTMASK)) ||
                        ((sr_ & SR_BRKINT) && (sr_ & SR_BRKINTMASK)) || ((cvr_ & CVR_INT) && (cvr_ & CVR_INTMASK));
  hw_.set_interrupt(INT_DI, asserted);
}

}  // namespace hw
