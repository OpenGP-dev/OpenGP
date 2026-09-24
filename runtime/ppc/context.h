// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gekko register state shared by recompiled code and the interpreter.
#pragma once

#include <bit>
#include <cstdint>

#include "gekko/types.h"

namespace ppc {

using namespace gekko;  // integer typedefs (u8..s64)

// Special-purpose register numbers used by the runtime.
namespace spr {
inline constexpr u32 XER = 1;
inline constexpr u32 LR = 8;
inline constexpr u32 CTR = 9;
inline constexpr u32 DSISR = 18;
inline constexpr u32 DAR = 19;
inline constexpr u32 DEC = 22;
inline constexpr u32 SRR0 = 26;
inline constexpr u32 SRR1 = 27;
inline constexpr u32 TBL = 268;
inline constexpr u32 TBU = 269;
inline constexpr u32 TBL_W = 284;
inline constexpr u32 TBU_W = 285;
inline constexpr u32 PVR = 287;
inline constexpr u32 GQR0 = 912;
inline constexpr u32 HID2 = 920;
inline constexpr u32 WPAR = 921;
inline constexpr u32 DMAU = 922;
inline constexpr u32 DMAL = 923;
inline constexpr u32 HID0 = 1008;
inline constexpr u32 HID1 = 1009;
inline constexpr u32 L2CR = 1017;
}  // namespace spr

// HID2 bits (big-endian bit 0 = MSB).
inline constexpr u32 HID2_LSQE = 0x80000000;  // load/store quantized enable
inline constexpr u32 HID2_WPE = 0x40000000;   // write gather pipe enable
inline constexpr u32 HID2_PSE = 0x20000000;   // paired single enable
inline constexpr u32 HID2_LCE = 0x10000000;   // locked cache enable

inline constexpr u32 GEKKO_PVR = 0x00083214;

// One floating-point register: two lanes (paired singles). Stored as raw bit
// patterns so that moves preserve NaN payloads exactly.
struct PairedSingle {
  u64 ps0;
  u64 ps1;

  double d0() const { return std::bit_cast<double>(ps0); }
  double d1() const { return std::bit_cast<double>(ps1); }
  void set0(double v) { ps0 = std::bit_cast<u64>(v); }
  void set1(double v) { ps1 = std::bit_cast<u64>(v); }
  void set_both(double a, double b) {
    ps0 = std::bit_cast<u64>(a);
    ps1 = std::bit_cast<u64>(b);
  }
  void fill(double v) { ps0 = ps1 = std::bit_cast<u64>(v); }
};

struct Context {
  u32 gpr[32];
  PairedSingle fpr[32];

  u32 cr;   // 8 fields of 4 bits (LT GT EQ SO), field 0 in the top nibble
  u32 lr;
  u32 ctr;
  u8 xer_so;
  u8 xer_ov;
  u8 xer_ca;
  u16 xer_bc;  // byte count (bits 25-31) and byte-compare (bits 16-23) of XER
  u32 fpscr;
  u32 msr;

  // Address of the instruction being executed. Maintained by the interpreter;
  // recompiled code only sets it when calling out for diagnostics.
  u32 pc;

  // lwarx/stwcx. reservation.
  u32 reserve_addr;
  bool reserve;

  u32 sr[16];     // segment registers
  u32 spr[1024];  // every SPR not held in a dedicated field above
};

// Constructs a context in the state the Gekko has after reset + IPL.
void reset_context(Context& ctx);

}  // namespace ppc
