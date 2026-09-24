// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instruction semantics. One inline helper per non-branch instruction in
// common/gekko/include/gekko/ops.inc, taking the fields of its signature
// (sigs.inc) in order. Recompiled code calls these with constant operands,
// so after inlining they reduce to plain C++; the interpreter calls the same
// helpers, which keeps the two in lockstep by construction.
//
// Integer behaviour of edge cases (divide by zero etc.) follows Dolphin's
// interpreter, which is based on hardware tests.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

#include "ppc/context.h"
#include "ppc/fp.h"
#include "ppc/hooks.h"
#include "ppc/memory.h"

namespace ppc {

// ---- Condition register / XER helpers ------------------------------------------------------

inline u32 cr_field(const Context& c, u32 field) { return (c.cr >> (28 - 4 * field)) & 0xF; }

inline void set_cr_field(Context& c, u32 field, u32 value) {
  const u32 shift = 28 - 4 * field;
  c.cr = (c.cr & ~(0xFu << shift)) | ((value & 0xF) << shift);
}

inline u32 cr_bit(const Context& c, u32 bit) { return (c.cr >> (31 - bit)) & 1; }

inline void set_cr_bit(Context& c, u32 bit, u32 value) {
  const u32 m = 1u << (31 - bit);
  c.cr = (c.cr & ~m) | ((value & 1) << (31 - bit));
}

inline void update_cr0(Context& c, u32 result) {
  const s32 r = static_cast<s32>(result);
  const u32 v = (r < 0 ? 8u : r > 0 ? 4u : 2u) | c.xer_so;
  set_cr_field(c, 0, v);
}

inline void update_cr1(Context& c) { set_cr_field(c, 1, c.fpscr >> 28); }

inline u32 get_xer(const Context& c) {
  return (u32{c.xer_so} << 31) | (u32{c.xer_ov} << 30) | (u32{c.xer_ca} << 29) | c.xer_bc;
}

inline void set_xer(Context& c, u32 v) {
  c.xer_so = (v >> 31) & 1;
  c.xer_ov = (v >> 30) & 1;
  c.xer_ca = (v >> 29) & 1;
  c.xer_bc = static_cast<u16>(v & 0xFF7F);
}

inline void set_ov(Context& c, u32 ov) {
  c.xer_ov = static_cast<u8>(ov);
  c.xer_so |= static_cast<u8>(ov);
}

inline u32 rotl(u32 v, u32 n) { return std::rotl(v, static_cast<int>(n & 31)); }

inline constexpr u32 rotate_mask(u32 mb, u32 me) {
  const u32 begin = 0xFFFFFFFFu >> mb;
  const u32 end = 0x7FFFFFFFu >> me;
  const u32 mask = begin ^ end;
  return me < mb ? ~mask : mask;
}

// Branch condition for bc/bclr (may decrement CTR).
inline bool bc_cond(Context& c, u32 bo, u32 bi) {
  if (!(bo & 4)) --c.ctr;
  const bool ctr_ok = (bo & 4) || ((c.ctr != 0) ^ ((bo >> 1) & 1));
  const bool cond_ok = (bo & 16) || (cr_bit(c, bi) == ((bo >> 3) & 1));
  return ctr_ok && cond_ok;
}

// Branch condition for bcctr (never decrements CTR).
inline bool bcctr_cond(const Context& c, u32 bo, u32 bi) {
  return (bo & 16) || (cr_bit(c, bi) == ((bo >> 3) & 1));
}

namespace op {

namespace detail {

inline u32 ra0(const Context& c, u32 ra) { return ra ? c.gpr[ra] : 0; }

// x + y + carry_in with carry and signed overflow.
struct AddResult {
  u32 value;
  u32 ca;
  u32 ov;
};

inline AddResult add3(u32 x, u32 y, u32 cin) {
  const u64 wide = u64{x} + y + cin;
  const u32 r = static_cast<u32>(wide);
  return {r, static_cast<u32>(wide >> 32), ((x ^ r) & (y ^ r)) >> 31};
}

inline void finish_xo(Context& c, u32 rd, const AddResult& r, u32 oe, u32 rc, bool set_ca) {
  c.gpr[rd] = r.value;
  if (set_ca) c.xer_ca = static_cast<u8>(r.ca);
  if (oe) set_ov(c, r.ov);
  if (rc) update_cr0(c, r.value);
}

inline double d0(const Context& c, u32 r) { return c.fpr[r].d0(); }
inline double d1(const Context& c, u32 r) { return c.fpr[r].d1(); }

// Stores a single-precision result into both lanes (Gekko behaviour).
inline void fill_single(Context& c, u32 rd, float v) { c.fpr[rd].fill(static_cast<double>(v)); }

inline void set_ps(Context& c, u32 rd, float a, float b) {
  c.fpr[rd].set_both(static_cast<double>(a), static_cast<double>(b));
}

inline void fp_compare(Context& c, u32 crfd, double a, double b) {
  u32 cc;
  if (std::isnan(a) || std::isnan(b)) cc = 1;  // unordered
  else if (a < b) cc = 8;
  else if (a > b) cc = 4;
  else cc = 2;
  c.fpscr = (c.fpscr & ~fp::FPSCR_FPCC) | (cc << 12);
  set_cr_field(c, crfd, cc);
}

inline bool trap_condition(u32 to, u32 a, u32 b) {
  const s32 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
  return ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) ||
         ((to & 2) && a < b) || ((to & 1) && a > b);
}

// Quantized load/store helpers.
enum : u32 { QUANT_FLOAT = 0, QUANT_U8 = 4, QUANT_U16 = 5, QUANT_S8 = 6, QUANT_S16 = 7 };

template <typename T>
inline T scale_and_clamp(double ps, u32 scale) {
  float v = static_cast<float>(ps) * fp::quantize_table[scale];
  if (std::isnan(v)) return 0;
  constexpr float lo = static_cast<float>(std::numeric_limits<T>::min());
  constexpr float hi = static_cast<float>(std::numeric_limits<T>::max());
  v = std::clamp(v, lo, hi);
  return static_cast<T>(v);
}

template <Mem K>
inline void quantized_load(Context& c, u8* base, u32 ea, u32 frd, u32 w, u32 i) {
  const u32 gqr = c.spr[spr::GQR0 + i];
  const u32 type = (gqr >> 16) & 7;
  const u32 scale = (gqr >> 24) & 0x3F;
  double ps0 = 0.0, ps1 = 1.0;
  auto deq = [&](float v) { return static_cast<double>(v * fp::dequantize_table[scale]); };
  switch (type) {
    case QUANT_FLOAT:
      ps0 = std::bit_cast<double>(fp::convert_to_double(read<K, u32>(base, ea)));
      if (!w) ps1 = std::bit_cast<double>(fp::convert_to_double(read<K, u32>(base, ea + 4)));
      break;
    case QUANT_U8:
      ps0 = deq(static_cast<float>(read<K, u8>(base, ea)));
      if (!w) ps1 = deq(static_cast<float>(read<K, u8>(base, ea + 1)));
      break;
    case QUANT_U16:
      ps0 = deq(static_cast<float>(read<K, u16>(base, ea)));
      if (!w) ps1 = deq(static_cast<float>(read<K, u16>(base, ea + 2)));
      break;
    case QUANT_S8:
      ps0 = deq(static_cast<float>(static_cast<s8>(read<K, u8>(base, ea))));
      if (!w) ps1 = deq(static_cast<float>(static_cast<s8>(read<K, u8>(base, ea + 1))));
      break;
    case QUANT_S16:
      ps0 = deq(static_cast<float>(static_cast<s16>(read<K, u16>(base, ea))));
      if (!w) ps1 = deq(static_cast<float>(static_cast<s16>(read<K, u16>(base, ea + 2))));
      break;
    default:  // reserved types
      ps0 = 0.0;
      ps1 = 0.0;
      break;
  }
  c.fpr[frd].set_both(ps0, ps1);
}

template <Mem K>
inline void quantized_store(Context& c, u8* base, u32 ea, u32 frs, u32 w, u32 i) {
  const u32 gqr = c.spr[spr::GQR0 + i];
  const u32 type = gqr & 7;
  const u32 scale = (gqr >> 8) & 0x3F;
  const double ps0 = c.fpr[frs].d0(), ps1 = c.fpr[frs].d1();
  switch (type) {
    case QUANT_FLOAT:
      write<K, u32>(base, ea, fp::convert_to_single_ftz(c.fpr[frs].ps0));
      if (!w) write<K, u32>(base, ea + 4, fp::convert_to_single_ftz(c.fpr[frs].ps1));
      break;
    case QUANT_U8:
      write<K, u8>(base, ea, scale_and_clamp<u8>(ps0, scale));
      if (!w) write<K, u8>(base, ea + 1, scale_and_clamp<u8>(ps1, scale));
      break;
    case QUANT_U16:
      write<K, u16>(base, ea, scale_and_clamp<u16>(ps0, scale));
      if (!w) write<K, u16>(base, ea + 2, scale_and_clamp<u16>(ps1, scale));
      break;
    case QUANT_S8:
      write<K, u8>(base, ea, static_cast<u8>(scale_and_clamp<s8>(ps0, scale)));
      if (!w) write<K, u8>(base, ea + 1, static_cast<u8>(scale_and_clamp<s8>(ps1, scale)));
      break;
    case QUANT_S16:
      write<K, u16>(base, ea, static_cast<u16>(scale_and_clamp<s16>(ps0, scale)));
      if (!w) write<K, u16>(base, ea + 2, static_cast<u16>(scale_and_clamp<s16>(ps1, scale)));
      break;
    default:
      break;
  }
}

}  // namespace detail

using detail::ra0;

// ---- Integer arithmetic with immediates (D_rD_rA_SIMM) ------------------------------------

inline void addi(Context& c, u32 rd, u32 ra, s32 simm) { c.gpr[rd] = ra0(c, ra) + static_cast<u32>(simm); }
inline void addis(Context& c, u32 rd, u32 ra, s32 simm) { c.gpr[rd] = ra0(c, ra) + (static_cast<u32>(simm) << 16); }

inline void addic(Context& c, u32 rd, u32 ra, s32 simm) {
  const auto r = detail::add3(c.gpr[ra], static_cast<u32>(simm), 0);
  c.gpr[rd] = r.value;
  c.xer_ca = static_cast<u8>(r.ca);
}
inline void addic_rc(Context& c, u32 rd, u32 ra, s32 simm) {
  addic(c, rd, ra, simm);
  update_cr0(c, c.gpr[rd]);
}
inline void mulli(Context& c, u32 rd, u32 ra, s32 simm) {
  c.gpr[rd] = static_cast<u32>(static_cast<s64>(static_cast<s32>(c.gpr[ra])) * simm);
}
inline void subfic(Context& c, u32 rd, u32 ra, s32 simm) {
  const auto r = detail::add3(~c.gpr[ra], static_cast<u32>(simm), 1);
  c.gpr[rd] = r.value;
  c.xer_ca = static_cast<u8>(r.ca);
}

// ---- Logical immediates (D_rA_rS_UIMM) ---------------------------------------------------

inline void ori(Context& c, u32 ra, u32 rs, u32 uimm) { c.gpr[ra] = c.gpr[rs] | uimm; }
inline void oris(Context& c, u32 ra, u32 rs, u32 uimm) { c.gpr[ra] = c.gpr[rs] | (uimm << 16); }
inline void xori(Context& c, u32 ra, u32 rs, u32 uimm) { c.gpr[ra] = c.gpr[rs] ^ uimm; }
inline void xoris(Context& c, u32 ra, u32 rs, u32 uimm) { c.gpr[ra] = c.gpr[rs] ^ (uimm << 16); }
inline void andi_rc(Context& c, u32 ra, u32 rs, u32 uimm) {
  c.gpr[ra] = c.gpr[rs] & uimm;
  update_cr0(c, c.gpr[ra]);
}
inline void andis_rc(Context& c, u32 ra, u32 rs, u32 uimm) {
  c.gpr[ra] = c.gpr[rs] & (uimm << 16);
  update_cr0(c, c.gpr[ra]);
}

// ---- Compares --------------------------------------------------------------------------------

inline void compare_signed(Context& c, u32 crfd, s32 a, s32 b) {
  set_cr_field(c, crfd, (a < b ? 8u : a > b ? 4u : 2u) | c.xer_so);
}
inline void compare_unsigned(Context& c, u32 crfd, u32 a, u32 b) {
  set_cr_field(c, crfd, (a < b ? 8u : a > b ? 4u : 2u) | c.xer_so);
}
inline void cmpi(Context& c, u32 crfd, u32 ra, s32 simm) { compare_signed(c, crfd, static_cast<s32>(c.gpr[ra]), simm); }
inline void cmpli(Context& c, u32 crfd, u32 ra, u32 uimm) { compare_unsigned(c, crfd, c.gpr[ra], uimm); }
inline void cmp(Context& c, u32 crfd, u32 ra, u32 rb) {
  compare_signed(c, crfd, static_cast<s32>(c.gpr[ra]), static_cast<s32>(c.gpr[rb]));
}
inline void cmpl(Context& c, u32 crfd, u32 ra, u32 rb) { compare_unsigned(c, crfd, c.gpr[ra], c.gpr[rb]); }

// ---- Traps -------------------------------------------------------------------------------------

inline void twi(Context& c, u8* base, u32 pc, u32 to, u32 ra, s32 simm) {
  if (detail::trap_condition(to, c.gpr[ra], static_cast<u32>(simm))) [[unlikely]]
    g_hooks.trap(c, base, pc);
}
inline void tw(Context& c, u8* base, u32 pc, u32 to, u32 ra, u32 rb) {
  if (detail::trap_condition(to, c.gpr[ra], c.gpr[rb])) [[unlikely]]
    g_hooks.trap(c, base, pc);
}

// ---- XO-form arithmetic ---------------------------------------------------------------------

inline void add(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(c.gpr[ra], c.gpr[rb], 0), oe, rc, false);
}
inline void addc(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(c.gpr[ra], c.gpr[rb], 0), oe, rc, true);
}
inline void adde(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(c.gpr[ra], c.gpr[rb], c.xer_ca), oe, rc, true);
}
inline void subf(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], c.gpr[rb], 1), oe, rc, false);
}
inline void subfc(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], c.gpr[rb], 1), oe, rc, true);
}
inline void subfe(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], c.gpr[rb], c.xer_ca), oe, rc, true);
}
inline void neg(Context& c, u32 rd, u32 ra, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], 0, 1), oe, rc, false);
}
inline void addze(Context& c, u32 rd, u32 ra, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(c.gpr[ra], 0, c.xer_ca), oe, rc, true);
}
inline void addme(Context& c, u32 rd, u32 ra, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(c.gpr[ra], 0xFFFFFFFF, c.xer_ca), oe, rc, true);
}
inline void subfze(Context& c, u32 rd, u32 ra, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], 0, c.xer_ca), oe, rc, true);
}
inline void subfme(Context& c, u32 rd, u32 ra, u32 oe, u32 rc) {
  detail::finish_xo(c, rd, detail::add3(~c.gpr[ra], 0xFFFFFFFF, c.xer_ca), oe, rc, true);
}
inline void mullw(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  const s64 p = static_cast<s64>(static_cast<s32>(c.gpr[ra])) * static_cast<s32>(c.gpr[rb]);
  c.gpr[rd] = static_cast<u32>(p);
  if (oe) set_ov(c, p != static_cast<s32>(p));
  if (rc) update_cr0(c, c.gpr[rd]);
}
inline void mulhw(Context& c, u32 rd, u32 ra, u32 rb, u32 rc) {
  const s64 p = static_cast<s64>(static_cast<s32>(c.gpr[ra])) * static_cast<s32>(c.gpr[rb]);
  c.gpr[rd] = static_cast<u32>(static_cast<u64>(p) >> 32);
  if (rc) update_cr0(c, c.gpr[rd]);
}
inline void mulhwu(Context& c, u32 rd, u32 ra, u32 rb, u32 rc) {
  c.gpr[rd] = static_cast<u32>((u64{c.gpr[ra]} * c.gpr[rb]) >> 32);
  if (rc) update_cr0(c, c.gpr[rd]);
}
inline void divw(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  const s32 a = static_cast<s32>(c.gpr[ra]);
  const s32 b = static_cast<s32>(c.gpr[rb]);
  const bool overflow = b == 0 || (static_cast<u32>(a) == 0x80000000u && b == -1);
  if (overflow) c.gpr[rd] = a < 0 ? 0xFFFFFFFFu : 0;
  else c.gpr[rd] = static_cast<u32>(a / b);
  if (oe) set_ov(c, overflow);
  if (rc) update_cr0(c, c.gpr[rd]);
}
inline void divwu(Context& c, u32 rd, u32 ra, u32 rb, u32 oe, u32 rc) {
  const u32 a = c.gpr[ra], b = c.gpr[rb];
  const bool overflow = b == 0;
  c.gpr[rd] = overflow ? 0 : a / b;
  if (oe) set_ov(c, overflow);
  if (rc) update_cr0(c, c.gpr[rd]);
}

// ---- Logical / shifts / rotates -----------------------------------------------------------

#define PPC_LOGICAL(NAME, EXPR)                                         \
  inline void NAME(Context& c, u32 ra, u32 rs, u32 rb, u32 rc) {       \
    const u32 s = c.gpr[rs], b = c.gpr[rb];                             \
    c.gpr[ra] = (EXPR);                                                 \
    if (rc) update_cr0(c, c.gpr[ra]);                                   \
  }
PPC_LOGICAL(and_, s & b)
PPC_LOGICAL(andc, s & ~b)
PPC_LOGICAL(or_, s | b)
PPC_LOGICAL(orc, s | ~b)
PPC_LOGICAL(xor_, s ^ b)
PPC_LOGICAL(nor, ~(s | b))
PPC_LOGICAL(nand, ~(s & b))
PPC_LOGICAL(eqv, ~(s ^ b))
PPC_LOGICAL(slw, (b & 0x20) ? 0u : (s << (b & 31)))
PPC_LOGICAL(srw, (b & 0x20) ? 0u : (s >> (b & 31)))
#undef PPC_LOGICAL

inline void sraw(Context& c, u32 ra, u32 rs, u32 rb, u32 rc) {
  const u32 n = c.gpr[rb] & 0x3F;
  const s32 s = static_cast<s32>(c.gpr[rs]);
  if (n & 0x20) {
    c.gpr[ra] = s < 0 ? 0xFFFFFFFFu : 0;
    c.xer_ca = s < 0;
  } else {
    c.gpr[ra] = static_cast<u32>(s >> n);
    c.xer_ca = s < 0 && (static_cast<u32>(s) & ((1u << n) - 1)) != 0;
  }
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void srawi(Context& c, u32 ra, u32 rs, u32 sh, u32 rc) {
  const s32 s = static_cast<s32>(c.gpr[rs]);
  c.gpr[ra] = static_cast<u32>(s >> sh);
  c.xer_ca = s < 0 && (static_cast<u32>(s) & ((1u << sh) - 1)) != 0;
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void cntlzw(Context& c, u32 ra, u32 rs, u32 rc) {
  c.gpr[ra] = static_cast<u32>(std::countl_zero(c.gpr[rs]));
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void extsb(Context& c, u32 ra, u32 rs, u32 rc) {
  c.gpr[ra] = static_cast<u32>(static_cast<s32>(static_cast<s8>(c.gpr[rs])));
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void extsh(Context& c, u32 ra, u32 rs, u32 rc) {
  c.gpr[ra] = static_cast<u32>(static_cast<s32>(static_cast<s16>(c.gpr[rs])));
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void rlwinm(Context& c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me, u32 rc) {
  c.gpr[ra] = rotl(c.gpr[rs], sh) & rotate_mask(mb, me);
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void rlwimi(Context& c, u32 ra, u32 rs, u32 sh, u32 mb, u32 me, u32 rc) {
  const u32 m = rotate_mask(mb, me);
  c.gpr[ra] = (rotl(c.gpr[rs], sh) & m) | (c.gpr[ra] & ~m);
  if (rc) update_cr0(c, c.gpr[ra]);
}
inline void rlwnm(Context& c, u32 ra, u32 rs, u32 rb, u32 mb, u32 me, u32 rc) {
  c.gpr[ra] = rotl(c.gpr[rs], c.gpr[rb] & 31) & rotate_mask(mb, me);
  if (rc) update_cr0(c, c.gpr[ra]);
}

// ---- Integer loads and stores -------------------------------------------------------------

#define PPC_LOAD(NAME, T, CONV)                                                            \
  template <Mem K>                                                                         \
  inline void NAME(Context& c, u8* base, u32 rd, u32 ra, s32 d) {                          \
    c.gpr[rd] = CONV(read<K, T>(base, ra0(c, ra) + static_cast<u32>(d)));                  \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##u(Context& c, u8* base, u32 rd, u32 ra, s32 d) {                       \
    const u32 ea = c.gpr[ra] + static_cast<u32>(d);                                        \
    c.gpr[rd] = CONV(read<K, T>(base, ea));                                                \
    c.gpr[ra] = ea;                                                                        \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##x(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {                      \
    c.gpr[rd] = CONV(read<K, T>(base, ra0(c, ra) + c.gpr[rb]));                            \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##ux(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {                     \
    const u32 ea = c.gpr[ra] + c.gpr[rb];                                                  \
    c.gpr[rd] = CONV(read<K, T>(base, ea));                                                \
    c.gpr[ra] = ea;                                                                        \
  }
namespace detail {
template <typename T>
inline u32 zext(T v) { return static_cast<u32>(v); }
inline u32 sext16(u16 v) { return static_cast<u32>(static_cast<s32>(static_cast<s16>(v))); }
}  // namespace detail
PPC_LOAD(lwz, u32, detail::zext)
PPC_LOAD(lbz, u8, detail::zext)
PPC_LOAD(lhz, u16, detail::zext)
PPC_LOAD(lha, u16, detail::sext16)
#undef PPC_LOAD

#define PPC_STORE(NAME, T)                                                                 \
  template <Mem K>                                                                         \
  inline void NAME(Context& c, u8* base, u32 rs, u32 ra, s32 d) {                          \
    write<K, T>(base, ra0(c, ra) + static_cast<u32>(d), static_cast<T>(c.gpr[rs]));        \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##u(Context& c, u8* base, u32 rs, u32 ra, s32 d) {                       \
    const u32 ea = c.gpr[ra] + static_cast<u32>(d);                                        \
    write<K, T>(base, ea, static_cast<T>(c.gpr[rs]));                                      \
    c.gpr[ra] = ea;                                                                        \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##x(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {                      \
    write<K, T>(base, ra0(c, ra) + c.gpr[rb], static_cast<T>(c.gpr[rs]));                  \
  }                                                                                        \
  template <Mem K>                                                                         \
  inline void NAME##ux(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {                     \
    const u32 ea = c.gpr[ra] + c.gpr[rb];                                                  \
    write<K, T>(base, ea, static_cast<T>(c.gpr[rs]));                                      \
    c.gpr[ra] = ea;                                                                        \
  }
PPC_STORE(stw, u32)
PPC_STORE(stb, u8)
PPC_STORE(sth, u16)
#undef PPC_STORE

template <Mem K>
inline void lhbrx(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {
  c.gpr[rd] = byteswap_value(read<K, u16>(base, ra0(c, ra) + c.gpr[rb]));
}
template <Mem K>
inline void lwbrx(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {
  c.gpr[rd] = byteswap_value(read<K, u32>(base, ra0(c, ra) + c.gpr[rb]));
}
template <Mem K>
inline void sthbrx(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {
  write<K, u16>(base, ra0(c, ra) + c.gpr[rb], byteswap_value(static_cast<u16>(c.gpr[rs])));
}
template <Mem K>
inline void stwbrx(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {
  write<K, u32>(base, ra0(c, ra) + c.gpr[rb], byteswap_value(c.gpr[rs]));
}

template <Mem K>
inline void lmw(Context& c, u8* base, u32 rd, u32 ra, s32 d) {
  u32 ea = ra0(c, ra) + static_cast<u32>(d);
  for (u32 r = rd; r < 32; ++r, ea += 4) c.gpr[r] = read<K, u32>(base, ea);
}
template <Mem K>
inline void stmw(Context& c, u8* base, u32 rs, u32 ra, s32 d) {
  u32 ea = ra0(c, ra) + static_cast<u32>(d);
  for (u32 r = rs; r < 32; ++r, ea += 4) write<K, u32>(base, ea, c.gpr[r]);
}

template <Mem K>
inline void lwarx(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {
  const u32 ea = ra0(c, ra) + c.gpr[rb];
  c.gpr[rd] = read<K, u32>(base, ea);
  c.reserve = true;
  c.reserve_addr = ea;
}
template <Mem K>
inline void stwcx_rc(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {
  const u32 ea = ra0(c, ra) + c.gpr[rb];
  u32 f = c.xer_so;
  if (c.reserve && c.reserve_addr == ea) {
    write<K, u32>(base, ea, c.gpr[rs]);
    f |= 2;
  }
  c.reserve = false;
  set_cr_field(c, 0, f);
}

// External control (eciwx/ecowx): treated as plain word accesses.
template <Mem K>
inline void eciwx(Context& c, u8* base, u32 rd, u32 ra, u32 rb) { lwzx<K>(c, base, rd, ra, rb); }
template <Mem K>
inline void ecowx(Context& c, u8* base, u32 rs, u32 ra, u32 rb) { stwx<K>(c, base, rs, ra, rb); }

namespace detail {
template <Mem K>
inline void load_string(Context& c, u8* base, u32 rd, u32 ea, u32 n) {
  u32 r = (rd + 31) & 31;
  u32 i = 0;
  while (n > 0) {
    if (i == 0) {
      r = (r + 1) & 31;
      c.gpr[r] = 0;
    }
    c.gpr[r] |= u32{read<K, u8>(base, ea)} << (24 - i);
    i = (i + 8) & 31;
    ++ea;
    --n;
  }
}
template <Mem K>
inline void store_string(Context& c, u8* base, u32 rs, u32 ea, u32 n) {
  u32 r = (rs + 31) & 31;
  u32 i = 0;
  while (n > 0) {
    if (i == 0) r = (r + 1) & 31;
    write<K, u8>(base, ea, static_cast<u8>(c.gpr[r] >> (24 - i)));
    i = (i + 8) & 31;
    ++ea;
    --n;
  }
}
}  // namespace detail

template <Mem K>
inline void lswi(Context& c, u8* base, u32 rd, u32 ra, u32 nb) {
  detail::load_string<K>(c, base, rd, ra0(c, ra), nb ? nb : 32);
}
template <Mem K>
inline void stswi(Context& c, u8* base, u32 rs, u32 ra, u32 nb) {
  detail::store_string<K>(c, base, rs, ra0(c, ra), nb ? nb : 32);
}
template <Mem K>
inline void lswx(Context& c, u8* base, u32 rd, u32 ra, u32 rb) {
  detail::load_string<K>(c, base, rd, ra0(c, ra) + c.gpr[rb], c.xer_bc & 0x7F);
}
template <Mem K>
inline void stswx(Context& c, u8* base, u32 rs, u32 ra, u32 rb) {
  detail::store_string<K>(c, base, rs, ra0(c, ra) + c.gpr[rb], c.xer_bc & 0x7F);
}

// ---- Floating-point loads and stores ------------------------------------------------------

#define PPC_FLOAD(NAME, ...)                                                                \
  template <Mem K>                                                                          \
  inline void NAME(Context& c, u8* base, u32 frd, u32 ra, s32 d) {                          \
    const u32 ea = ra0(c, ra) + static_cast<u32>(d);                                        \
    __VA_ARGS__;                                                                            \
  }                                                                                         \
  template <Mem K>                                                                          \
  inline void NAME##u(Context& c, u8* base, u32 frd, u32 ra, s32 d) {                       \
    const u32 ea = c.gpr[ra] + static_cast<u32>(d);                                         \
    __VA_ARGS__;                                                                            \
    c.gpr[ra] = ea;                                                                         \
  }                                                                                         \
  template <Mem K>                                                                          \
  inline void NAME##x(Context& c, u8* base, u32 frd, u32 ra, u32 rb) {                      \
    const u32 ea = ra0(c, ra) + c.gpr[rb];                                                  \
    __VA_ARGS__;                                                                            \
  }                                                                                         \
  template <Mem K>                                                                          \
  inline void NAME##ux(Context& c, u8* base, u32 frd, u32 ra, u32 rb) {                     \
    const u32 ea = c.gpr[ra] + c.gpr[rb];                                                   \
    __VA_ARGS__;                                                                            \
    c.gpr[ra] = ea;                                                                         \
  }
// lfs fills both lanes on the Gekko; lfd only writes ps0.
PPC_FLOAD(lfs, c.fpr[frd].ps0 = c.fpr[frd].ps1 = fp::convert_to_double(read<K, u32>(base, ea)))
PPC_FLOAD(lfd, c.fpr[frd].ps0 = read<K, u64>(base, ea))
PPC_FLOAD(stfs, write<K, u32>(base, ea, fp::convert_to_single(c.fpr[frd].ps0)))
PPC_FLOAD(stfd, write<K, u64>(base, ea, c.fpr[frd].ps0))
#undef PPC_FLOAD

template <Mem K>
inline void stfiwx(Context& c, u8* base, u32 frs, u32 ra, u32 rb) {
  write<K, u32>(base, ra0(c, ra) + c.gpr[rb], static_cast<u32>(c.fpr[frs].ps0));
}

// ---- Quantized loads and stores -------------------------------------------------------------

template <Mem K>
inline void psq_l(Context& c, u8* base, u32 frd, u32 ra, s32 d, u32 w, u32 i) {
  detail::quantized_load<K>(c, base, ra0(c, ra) + static_cast<u32>(d), frd, w, i);
}
template <Mem K>
inline void psq_lu(Context& c, u8* base, u32 frd, u32 ra, s32 d, u32 w, u32 i) {
  const u32 ea = c.gpr[ra] + static_cast<u32>(d);
  detail::quantized_load<K>(c, base, ea, frd, w, i);
  c.gpr[ra] = ea;
}
template <Mem K>
inline void psq_lx(Context& c, u8* base, u32 frd, u32 ra, u32 rb, u32 w, u32 i) {
  detail::quantized_load<K>(c, base, ra0(c, ra) + c.gpr[rb], frd, w, i);
}
template <Mem K>
inline void psq_lux(Context& c, u8* base, u32 frd, u32 ra, u32 rb, u32 w, u32 i) {
  const u32 ea = c.gpr[ra] + c.gpr[rb];
  detail::quantized_load<K>(c, base, ea, frd, w, i);
  c.gpr[ra] = ea;
}
template <Mem K>
inline void psq_st(Context& c, u8* base, u32 frs, u32 ra, s32 d, u32 w, u32 i) {
  detail::quantized_store<K>(c, base, ra0(c, ra) + static_cast<u32>(d), frs, w, i);
}
template <Mem K>
inline void psq_stu(Context& c, u8* base, u32 frs, u32 ra, s32 d, u32 w, u32 i) {
  const u32 ea = c.gpr[ra] + static_cast<u32>(d);
  detail::quantized_store<K>(c, base, ea, frs, w, i);
  c.gpr[ra] = ea;
}
template <Mem K>
inline void psq_stx(Context& c, u8* base, u32 frs, u32 ra, u32 rb, u32 w, u32 i) {
  detail::quantized_store<K>(c, base, ra0(c, ra) + c.gpr[rb], frs, w, i);
}
template <Mem K>
inline void psq_stux(Context& c, u8* base, u32 frs, u32 ra, u32 rb, u32 w, u32 i) {
  const u32 ea = c.gpr[ra] + c.gpr[rb];
  detail::quantized_store<K>(c, base, ea, frs, w, i);
  c.gpr[ra] = ea;
}

// ---- Cache control ----------------------------------------------------------------------------

template <Mem K>
inline void dcbz(Context& c, u8* base, u32 ra, u32 rb) {
  const u32 ea = (ra0(c, ra) + c.gpr[rb]) & ~31u;
  for (u32 i = 0; i < 32; i += 8) write<K, u64>(base, ea + i, 0);
}
// Zeroes a line of the locked cache (the address is the line's locked-cache address).
template <Mem K>
inline void dcbz_l(Context& c, u8* base, u32 ra, u32 rb) {
  dcbz<K>(c, base, ra, rb);
}
inline void dcbst(Context&, u8*, u32, u32) {}
inline void dcbf(Context&, u8*, u32, u32) {}
inline void dcbi(Context&, u8*, u32, u32) {}
inline void dcbt(Context&, u8*, u32, u32) {}
inline void dcbtst(Context&, u8*, u32, u32) {}
inline void icbi(Context& c, u8*, u32 ra, u32 rb) {
  if (g_hooks.icache_invalidate) g_hooks.icache_invalidate((ra0(c, ra) + c.gpr[rb]) & ~31u);
}
inline void sync(Context&) {}
inline void isync(Context&) {}
inline void eieio(Context&) {}
inline void tlbsync(Context&) {}
inline void tlbie(Context&, u32) {}

// ---- Condition register --------------------------------------------------------------------

#define PPC_CRBIT(NAME, EXPR)                                        \
  inline void NAME(Context& c, u32 d, u32 a, u32 b) {                \
    const u32 x = cr_bit(c, a), y = cr_bit(c, b);                    \
    set_cr_bit(c, d, (EXPR) & 1);                                    \
  }
PPC_CRBIT(crand, x & y)
PPC_CRBIT(crandc, x & ~y)
PPC_CRBIT(creqv, ~(x ^ y))
PPC_CRBIT(crnand, ~(x & y))
PPC_CRBIT(crnor, ~(x | y))
PPC_CRBIT(cror, x | y)
PPC_CRBIT(crorc, x | ~y)
PPC_CRBIT(crxor, x ^ y)
#undef PPC_CRBIT

inline void mcrf(Context& c, u32 crfd, u32 crfs) { set_cr_field(c, crfd, cr_field(c, crfs)); }
inline void mcrxr(Context& c, u32 crfd) {
  set_cr_field(c, crfd, (u32{c.xer_so} << 3) | (u32{c.xer_ov} << 2) | (u32{c.xer_ca} << 1));
  c.xer_so = c.xer_ov = c.xer_ca = 0;
}
inline void mfcr(Context& c, u32 rd) { c.gpr[rd] = c.cr; }
inline void mtcrf(Context& c, u32 crm, u32 rs) {
  u32 mask = 0;
  for (u32 i = 0; i < 8; ++i)
    if (crm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
  c.cr = (c.cr & ~mask) | (c.gpr[rs] & mask);
}

// ---- Special-purpose and supervisor registers -------------------------------------------

void locked_cache_dma(Context& c, u8* base);  // ops.cpp

inline u32 read_spr(Context& c, u32 n) {
  switch (n) {
    case spr::XER: return get_xer(c);
    case spr::LR: return c.lr;
    case spr::CTR: return c.ctr;
    case spr::DEC: return g_hooks.read_decrementer();
    case spr::TBL: return static_cast<u32>(g_hooks.read_timebase());
    case spr::TBU: return static_cast<u32>(g_hooks.read_timebase() >> 32);
    default: return c.spr[n & 1023];
  }
}

inline void mfspr(Context& c, u8*, u32 rd, u32 n) { c.gpr[rd] = read_spr(c, n); }
inline void mftb(Context& c, u8*, u32 rd, u32 n) {
  const u64 tb = g_hooks.read_timebase();
  c.gpr[rd] = n == spr::TBU ? static_cast<u32>(tb >> 32) : static_cast<u32>(tb);
}

inline void mtspr(Context& c, u8* base, u32 n, u32 rs) {
  const u32 v = c.gpr[rs];
  switch (n) {
    case spr::XER: set_xer(c, v); return;
    case spr::LR: c.lr = v; return;
    case spr::CTR: c.ctr = v; return;
    case spr::DEC: g_hooks.write_decrementer(v); return;
    case spr::TBL_W: {
      const u64 tb = g_hooks.read_timebase();
      g_hooks.write_timebase((tb & 0xFFFFFFFF00000000ULL) | v);
      return;
    }
    case spr::TBU_W: {
      const u64 tb = g_hooks.read_timebase();
      g_hooks.write_timebase((tb & 0xFFFFFFFFULL) | (u64{v} << 32));
      return;
    }
    case spr::PVR: return;  // read-only
    case spr::HID1: c.spr[n] = v & 0xF8000000u; return;
    case spr::HID2:
      // Only the upper half is writable, except the DMA queue length field.
      c.spr[n] = (v & 0xF0FF0000u) | (c.spr[n] & 0x0F000000u);
      return;
    case spr::DMAL:
      c.spr[n] = v;
      if (v & 2) locked_cache_dma(c, base);
      return;
    default: c.spr[n & 1023] = v; return;
  }
}

inline void mfmsr(Context& c, u32 rd) { c.gpr[rd] = c.msr; }
inline void mtmsr(Context& c, u8* base, u32 rs) {
  const u32 old = c.msr;
  c.msr = c.gpr[rs];
  if (g_hooks.msr_changed) g_hooks.msr_changed(c, base, old);
}
inline void mfsr(Context& c, u32 rd, u32 sr) { c.gpr[rd] = c.sr[sr & 15]; }
inline void mtsr(Context& c, u32 sr, u32 rs) { c.sr[sr & 15] = c.gpr[rs]; }
inline void mfsrin(Context& c, u32 rd, u32 rb) { c.gpr[rd] = c.sr[c.gpr[rb] >> 28]; }
inline void mtsrin(Context& c, u32 rs, u32 rb) { c.sr[c.gpr[rb] >> 28] = c.gpr[rs]; }

// ---- Floating point: moves and sign ops (X_FB) -------------------------------------------

#define PPC_FRC(c, rc) \
  if (rc) update_cr1(c)

inline void fmr(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].ps0 = c.fpr[frb].ps0;
  PPC_FRC(c, rc);
}
inline void fneg(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].ps0 = c.fpr[frb].ps0 ^ fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}
inline void fabs_(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].ps0 = c.fpr[frb].ps0 & ~fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}
inline void fnabs(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].ps0 = c.fpr[frb].ps0 | fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}
inline void ps_mr(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd] = c.fpr[frb];
  PPC_FRC(c, rc);
}
inline void ps_neg(Context& c, u32 frd, u32 frb, u32 rc) {
  const PairedSingle b = c.fpr[frb];
  c.fpr[frd].ps0 = b.ps0 ^ fp::DOUBLE_SIGN;
  c.fpr[frd].ps1 = b.ps1 ^ fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}
inline void ps_abs(Context& c, u32 frd, u32 frb, u32 rc) {
  const PairedSingle b = c.fpr[frb];
  c.fpr[frd].ps0 = b.ps0 & ~fp::DOUBLE_SIGN;
  c.fpr[frd].ps1 = b.ps1 & ~fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}
inline void ps_nabs(Context& c, u32 frd, u32 frb, u32 rc) {
  const PairedSingle b = c.fpr[frb];
  c.fpr[frd].ps0 = b.ps0 | fp::DOUBLE_SIGN;
  c.fpr[frd].ps1 = b.ps1 | fp::DOUBLE_SIGN;
  PPC_FRC(c, rc);
}

inline void frsp(Context& c, u32 frd, u32 frb, u32 rc) {
  detail::fill_single(c, frd, fp::force_single(c.fpscr, detail::d0(c, frb)));
  PPC_FRC(c, rc);
}

namespace detail {
inline void convert_to_integer(Context& c, u32 frd, u32 frb, u32 rounding) {
  const double b = d0(c, frb);
  double rounded;
  switch (rounding) {
    case 0: {  // nearest, ties to even (host is in round-to-nearest)
      const double int_precision = std::copysign(4503599627370496.0, b);
      rounded = (b + int_precision) - int_precision;
      break;
    }
    case 1: rounded = std::trunc(b); break;
    case 2: rounded = std::ceil(b); break;
    default: rounded = std::floor(b); break;
  }
  u32 value;
  if (std::isnan(b)) value = 0x80000000u;
  else if (rounded >= static_cast<double>(0x80000000u)) value = 0x7FFFFFFFu;
  else if (rounded < -static_cast<double>(0x80000000u)) value = 0x80000000u;
  else value = static_cast<u32>(static_cast<s32>(rounded));
  // Upper word is 0xFFF80000 (0xFFF80001 for -0.0), per hardware tests.
  u64 result = 0xFFF8000000000000ULL | value;
  if (value == 0 && std::signbit(b)) result |= 0x100000000ULL;
  c.fpr[frd].ps0 = result;
}
}  // namespace detail

inline void fctiw(Context& c, u32 frd, u32 frb, u32 rc) {
  detail::convert_to_integer(c, frd, frb, c.fpscr & fp::FPSCR_RN);
  PPC_FRC(c, rc);
}
inline void fctiwz(Context& c, u32 frd, u32 frb, u32 rc) {
  detail::convert_to_integer(c, frd, frb, 1);
  PPC_FRC(c, rc);
}

// ---- Floating point arithmetic (double: ps0 only; single: both lanes) -------------------

#define PPC_FARITH2(NAME, FN)                                                        \
  inline void NAME(Context& c, u32 frd, u32 fra, u32 frb, u32 rc) {                  \
    c.fpr[frd].set0(fp::force_double(c.fpscr, fp::FN(detail::d0(c, fra), detail::d0(c, frb)))); \
    PPC_FRC(c, rc);                                                                  \
  }                                                                                  \
  inline void NAME##s(Context& c, u32 frd, u32 fra, u32 frb, u32 rc) {               \
    detail::fill_single(c, frd,                                                      \
                        fp::force_single(c.fpscr, fp::FN(detail::d0(c, fra), detail::d0(c, frb)))); \
    PPC_FRC(c, rc);                                                                  \
  }
PPC_FARITH2(fadd, ni_add)
PPC_FARITH2(fsub, ni_sub)
PPC_FARITH2(fdiv, ni_div)
#undef PPC_FARITH2

inline void fmul(Context& c, u32 frd, u32 fra, u32 frc, u32 rc) {
  c.fpr[frd].set0(fp::force_double(c.fpscr, fp::ni_mul(detail::d0(c, fra), detail::d0(c, frc))));
  PPC_FRC(c, rc);
}
inline void fmuls(Context& c, u32 frd, u32 fra, u32 frc, u32 rc) {
  const double cv = fp::force_25bit(detail::d0(c, frc));
  detail::fill_single(c, frd, fp::force_single(c.fpscr, fp::ni_mul(detail::d0(c, fra), cv)));
  PPC_FRC(c, rc);
}

#define PPC_FMADD(NAME, SUB, NEG)                                                               \
  inline void NAME(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {                    \
    double r = fp::force_double(                                                                \
        c.fpscr, fp::ni_madd_msub<SUB, false>(detail::d0(c, fra), detail::d0(c, frc), detail::d0(c, frb))); \
    if (NEG && !std::isnan(r)) r = -r;                                                          \
    c.fpr[frd].set0(r);                                                                         \
    PPC_FRC(c, rc);                                                                             \
  }                                                                                             \
  inline void NAME##s(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {                 \
    float r = fp::force_single(                                                                 \
        c.fpscr, fp::ni_madd_msub<SUB, true>(detail::d0(c, fra), detail::d0(c, frc), detail::d0(c, frb))); \
    if (NEG && !std::isnan(r)) r = -r;                                                          \
    detail::fill_single(c, frd, r);                                                             \
    PPC_FRC(c, rc);                                                                             \
  }
PPC_FMADD(fmadd, false, false)
PPC_FMADD(fmsub, true, false)
PPC_FMADD(fnmadd, false, true)
PPC_FMADD(fnmsub, true, true)
#undef PPC_FMADD

inline void fsel(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  c.fpr[frd].ps0 = detail::d0(c, fra) >= -0.0 ? c.fpr[frc].ps0 : c.fpr[frb].ps0;
  PPC_FRC(c, rc);
}
inline void fres(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].fill(fp::approximate_reciprocal(detail::d0(c, frb)));
  PPC_FRC(c, rc);
}
inline void frsqrte(Context& c, u32 frd, u32 frb, u32 rc) {
  c.fpr[frd].set0(fp::approximate_reciprocal_square_root(detail::d0(c, frb)));
  PPC_FRC(c, rc);
}

inline void fcmpu(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d0(c, fra), detail::d0(c, frb));
}
inline void fcmpo(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d0(c, fra), detail::d0(c, frb));
}

// ---- FPSCR ---------------------------------------------------------------------------------

inline void mffs(Context& c, u32 frd, u32 rc) {
  c.fpr[frd].ps0 = 0xFFF8000000000000ULL | c.fpscr;
  PPC_FRC(c, rc);
}
inline void mtfsb0(Context& c, u32 crbd, u32 rc) {
  c.fpscr = fp::update_summary(c.fpscr & ~(0x80000000u >> crbd));
  PPC_FRC(c, rc);
}
inline void mtfsb1(Context& c, u32 crbd, u32 rc) {
  const u32 b = 0x80000000u >> crbd;
  if ((b & fp::FPSCR_ANY_X) && !(c.fpscr & b)) c.fpscr |= fp::FPSCR_FX;
  c.fpscr = fp::update_summary(c.fpscr | b);
  PPC_FRC(c, rc);
}
inline void mtfsfi(Context& c, u32 crfd, u32 imm, u32 rc) {
  const u32 shift = 4 * (7 - crfd);
  c.fpscr = fp::update_summary((c.fpscr & ~(0xFu << shift)) | (imm << shift));
  PPC_FRC(c, rc);
}
inline void mtfsf(Context& c, u32 fm, u32 frb, u32 rc) {
  u32 mask = 0;
  for (u32 i = 0; i < 8; ++i)
    if (fm & (0x80u >> i)) mask |= 0xF0000000u >> (4 * i);
  c.fpscr = fp::update_summary((c.fpscr & ~mask) | (static_cast<u32>(c.fpr[frb].ps0) & mask));
  PPC_FRC(c, rc);
}
inline void mcrfs(Context& c, u32 crfd, u32 crfs) {
  const u32 shift = 4 * (7 - crfs);
  const u32 flags = (c.fpscr >> shift) & 0xF;
  c.fpscr = fp::update_summary(c.fpscr & ~((0xFu << shift) & (fp::FPSCR_FX | fp::FPSCR_ANY_X)));
  set_cr_field(c, crfd, flags);
}

// ---- Paired singles --------------------------------------------------------------------------

#define PPC_PS2(NAME, FN)                                                                      \
  inline void NAME(Context& c, u32 frd, u32 fra, u32 frb, u32 rc) {                           \
    const float r0 = fp::force_single(c.fpscr, fp::FN(detail::d0(c, fra), detail::d0(c, frb))); \
    const float r1 = fp::force_single(c.fpscr, fp::FN(detail::d1(c, fra), detail::d1(c, frb))); \
    detail::set_ps(c, frd, r0, r1);                                                            \
    PPC_FRC(c, rc);                                                                            \
  }
PPC_PS2(ps_add, ni_add)
PPC_PS2(ps_sub, ni_sub)
PPC_PS2(ps_div, ni_div)
#undef PPC_PS2

inline void ps_mul(Context& c, u32 frd, u32 fra, u32 frc, u32 rc) {
  const double c0 = fp::force_25bit(detail::d0(c, frc));
  const double c1 = fp::force_25bit(detail::d1(c, frc));
  const float r0 = fp::force_single(c.fpscr, fp::ni_mul(detail::d0(c, fra), c0));
  const float r1 = fp::force_single(c.fpscr, fp::ni_mul(detail::d1(c, fra), c1));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_muls0(Context& c, u32 frd, u32 fra, u32 frc, u32 rc) {
  const double c0 = fp::force_25bit(detail::d0(c, frc));
  const float r0 = fp::force_single(c.fpscr, fp::ni_mul(detail::d0(c, fra), c0));
  const float r1 = fp::force_single(c.fpscr, fp::ni_mul(detail::d1(c, fra), c0));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_muls1(Context& c, u32 frd, u32 fra, u32 frc, u32 rc) {
  const double c1 = fp::force_25bit(detail::d1(c, frc));
  const float r0 = fp::force_single(c.fpscr, fp::ni_mul(detail::d0(c, fra), c1));
  const float r1 = fp::force_single(c.fpscr, fp::ni_mul(detail::d1(c, fra), c1));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}

namespace detail {
template <bool sub, bool neg>
inline float ps_fma_lane(u32 fpscr, double a, double cv, double b) {
  float r = fp::force_single(fpscr, fp::ni_madd_msub<sub, true>(a, cv, b));
  if (neg && !std::isnan(r)) r = -r;
  return r;
}
}  // namespace detail

#define PPC_PSMADD(NAME, SUB, NEG)                                                             \
  inline void NAME(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {                   \
    const float r0 = detail::ps_fma_lane<SUB, NEG>(c.fpscr, detail::d0(c, fra), detail::d0(c, frc), detail::d0(c, frb)); \
    const float r1 = detail::ps_fma_lane<SUB, NEG>(c.fpscr, detail::d1(c, fra), detail::d1(c, frc), detail::d1(c, frb)); \
    detail::set_ps(c, frd, r0, r1);                                                            \
    PPC_FRC(c, rc);                                                                            \
  }
PPC_PSMADD(ps_madd, false, false)
PPC_PSMADD(ps_msub, true, false)
PPC_PSMADD(ps_nmadd, false, true)
PPC_PSMADD(ps_nmsub, true, true)
#undef PPC_PSMADD

inline void ps_madds0(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  const double c0 = detail::d0(c, frc);
  const float r0 = detail::ps_fma_lane<false, false>(c.fpscr, detail::d0(c, fra), c0, detail::d0(c, frb));
  const float r1 = detail::ps_fma_lane<false, false>(c.fpscr, detail::d1(c, fra), c0, detail::d1(c, frb));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_madds1(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  const double c1 = detail::d1(c, frc);
  const float r0 = detail::ps_fma_lane<false, false>(c.fpscr, detail::d0(c, fra), c1, detail::d0(c, frb));
  const float r1 = detail::ps_fma_lane<false, false>(c.fpscr, detail::d1(c, fra), c1, detail::d1(c, frb));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_sum0(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  const float r0 = fp::force_single(c.fpscr, fp::ni_add(detail::d0(c, fra), detail::d1(c, frb)));
  const float r1 = fp::force_single(c.fpscr, detail::d1(c, frc));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_sum1(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  const float r0 = fp::force_single(c.fpscr, detail::d0(c, frc));
  const float r1 = fp::force_single(c.fpscr, fp::ni_add(detail::d0(c, fra), detail::d1(c, frb)));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_sel(Context& c, u32 frd, u32 fra, u32 frc, u32 frb, u32 rc) {
  const PairedSingle a = c.fpr[fra], b = c.fpr[frb], cc = c.fpr[frc];
  c.fpr[frd].ps0 = a.d0() >= -0.0 ? cc.ps0 : b.ps0;
  c.fpr[frd].ps1 = a.d1() >= -0.0 ? cc.ps1 : b.ps1;
  PPC_FRC(c, rc);
}
inline void ps_res(Context& c, u32 frd, u32 frb, u32 rc) {
  const double r0 = fp::approximate_reciprocal(detail::d0(c, frb));
  const double r1 = fp::approximate_reciprocal(detail::d1(c, frb));
  c.fpr[frd].set_both(r0, r1);
  PPC_FRC(c, rc);
}
inline void ps_rsqrte(Context& c, u32 frd, u32 frb, u32 rc) {
  const float r0 = fp::force_single(c.fpscr, fp::approximate_reciprocal_square_root(detail::d0(c, frb)));
  const float r1 = fp::force_single(c.fpscr, fp::approximate_reciprocal_square_root(detail::d1(c, frb)));
  detail::set_ps(c, frd, r0, r1);
  PPC_FRC(c, rc);
}

#define PPC_MERGE(NAME, A, B)                                               \
  inline void NAME(Context& c, u32 frd, u32 fra, u32 frb, u32 rc) {         \
    const u64 x = c.fpr[fra].A, y = c.fpr[frb].B;                           \
    c.fpr[frd].ps0 = x;                                                     \
    c.fpr[frd].ps1 = y;                                                     \
    PPC_FRC(c, rc);                                                         \
  }
PPC_MERGE(ps_merge00, ps0, ps0)
PPC_MERGE(ps_merge01, ps0, ps1)
PPC_MERGE(ps_merge10, ps1, ps0)
PPC_MERGE(ps_merge11, ps1, ps1)
#undef PPC_MERGE

inline void ps_cmpu0(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d0(c, fra), detail::d0(c, frb));
}
inline void ps_cmpo0(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d0(c, fra), detail::d0(c, frb));
}
inline void ps_cmpu1(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d1(c, fra), detail::d1(c, frb));
}
inline void ps_cmpo1(Context& c, u32 crfd, u32 fra, u32 frb) {
  detail::fp_compare(c, crfd, detail::d1(c, fra), detail::d1(c, frb));
}

#undef PPC_FRC

}  // namespace op
}  // namespace ppc
