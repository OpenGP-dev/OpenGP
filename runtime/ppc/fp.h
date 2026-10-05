// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gekko floating-point behaviour: bit-exact single<->double conversion, NaN
// propagation order, the 25-bit rounding of frC in single-precision
// multiplies, single-rounding FMA, and the fres/frsqrte estimate tables.
//
// Ported from the Dolphin emulator (Source/Core/Core/PowerPC/Interpreter/
// Interpreter_FPUtils.h and Source/Core/Common/FloatUtils.cpp),
// Copyright Dolphin Emulator Project, GPL-2.0-or-later. Dolphin derives this
// behaviour from hardware tests.
//
// Differences from Dolphin: FPSCR exception/status flags (FX, VX*, ZX, FI, FR,
// FPRF) are not tracked yet, so arithmetic never raises FP exceptions.
#pragma once

#include <bit>
#include <cmath>
#include <limits>

#include "ppc/context.h"

namespace ppc::fp {

inline constexpr u64 DOUBLE_SIGN = 0x8000000000000000ULL;
inline constexpr u64 DOUBLE_EXP = 0x7FF0000000000000ULL;
inline constexpr u64 DOUBLE_FRAC = 0x000FFFFFFFFFFFFFULL;
inline constexpr u64 DOUBLE_QBIT = 0x0008000000000000ULL;
inline constexpr u32 FLOAT_EXP = 0x7F800000;
inline constexpr u32 FLOAT_FRAC = 0x007FFFFF;

// FPSCR bits (LSB numbering).
inline constexpr u32 FPSCR_FX = 1u << 31;
inline constexpr u32 FPSCR_FEX = 1u << 30;
inline constexpr u32 FPSCR_VX = 1u << 29;
inline constexpr u32 FPSCR_OX = 1u << 28;
inline constexpr u32 FPSCR_UX = 1u << 27;
inline constexpr u32 FPSCR_ZX = 1u << 26;
inline constexpr u32 FPSCR_XX = 1u << 25;
inline constexpr u32 FPSCR_VX_ANY = (0x3Fu << 19) | (0x7u << 8);  // VXSNAN..VXVC, VXSOFT..VXCVI
inline constexpr u32 FPSCR_ANY_X = FPSCR_OX | FPSCR_UX | FPSCR_ZX | FPSCR_XX | FPSCR_VX_ANY;
inline constexpr u32 FPSCR_ANY_E = 0xF8;  // VE OE UE ZE XE
inline constexpr u32 FPSCR_NI = 1u << 2;
inline constexpr u32 FPSCR_RN = 3u;
inline constexpr u32 FPSCR_FPCC = 0xFu << 12;

inline constexpr double PPC_NAN = std::numeric_limits<double>::quiet_NaN();

inline bool is_snan(double d) {
  const u64 i = std::bit_cast<u64>(d);
  return (i & DOUBLE_EXP) == DOUBLE_EXP && (i & DOUBLE_FRAC) != 0 && (i & DOUBLE_QBIT) == 0;
}

inline double make_quiet(double d) { return std::bit_cast<double>(std::bit_cast<u64>(d) | DOUBLE_QBIT); }

inline double flush_to_zero(double d) {
  const u64 i = std::bit_cast<u64>(d);
  if ((i & DOUBLE_EXP) == 0) return std::bit_cast<double>(i & DOUBLE_SIGN);
  return d;
}

inline float flush_to_zero(float f) {
  const u32 i = std::bit_cast<u32>(f);
  if ((i & FLOAT_EXP) == 0) return std::bit_cast<float>(i & 0x80000000u);
  return f;
}

// Recomputes the FEX and VX summary bits after a direct FPSCR write.
inline u32 update_summary(u32 fpscr) {
  fpscr &= ~(FPSCR_FEX | FPSCR_VX);
  if (fpscr & FPSCR_VX_ANY) fpscr |= FPSCR_VX;
  if ((fpscr >> 22) & (fpscr & FPSCR_ANY_E)) fpscr |= FPSCR_FEX;
  return fpscr;
}

// A float the compiler must treat as an unknown float, so that a rounding to
// single precision is never folded away when the result is widened back to
// double: GCC 12 at -O3 dropped the double -> float -> double round trip of
// paired-single results in recompiled code (unrounded results, no overflow to
// infinity, NaN payloads not truncated).
inline float keep_single(float x) {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  __asm__("" : "+x"(x));
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
  __asm__("" : "+w"(x));
#endif
  return x;
}

// Rounds to single precision as the Gekko does, honouring FPSCR[NI].
inline float force_single(u32 fpscr, double value) {
  if (fpscr & FPSCR_NI) {
    // If the value before rounding is a subnormal single it is flushed to zero,
    // even if rounding would have made it normal.
    constexpr u64 smallest_normal_single = 0x3810000000000000;
    const u64 without_sign = std::bit_cast<u64>(value) & (DOUBLE_EXP | DOUBLE_FRAC);
    if (without_sign < smallest_normal_single) {
      const u32 flushed = static_cast<u32>((std::bit_cast<u64>(value) & DOUBLE_SIGN) >> 32);
      return std::bit_cast<float>(flushed);
    }
  }
  float x = keep_single(static_cast<float>(value));
  if (fpscr & FPSCR_NI) x = flush_to_zero(x);
  return x;
}

inline double force_double(u32 fpscr, double d) {
  if (fpscr & FPSCR_NI) d = flush_to_zero(d);
  return d;
}

// Single-precision multiplies round frC to a 25-bit mantissa first.
inline double force_25bit(double d) {
  u64 integral = std::bit_cast<u64>(d);
  const u64 exponent = integral & DOUBLE_EXP;
  const u64 fraction = integral & DOUBLE_FRAC;
  if (exponent == 0 && fraction != 0) {
    // Subnormals are "normalized" before rounding, which moves the rounding bit.
    s64 keep_mask = static_cast<s64>(0xFFFFFFFFF8000000ULL);
    u64 round = 0x8000000;
    const u32 shift = static_cast<u32>(std::countl_zero(fraction)) - (63 - 52);
    keep_mask >>= shift;
    round >>= shift;
    integral = (integral & static_cast<u64>(keep_mask)) + (integral & round);
  } else {
    integral = (integral & 0xFFFFFFFFF8000000ULL) + (integral & 0x8000000);
  }
  return std::bit_cast<double>(integral);
}

// Arithmetic with PowerPC NaN semantics: operands are checked for NaN in the
// order frA, frB, frC; an invalid operation produces the default QNaN.
inline double ni_add(double a, double b) {
  const double r = a + b;
  if (std::isnan(r)) [[unlikely]] {
    if (std::isnan(a)) return make_quiet(a);
    if (std::isnan(b)) return make_quiet(b);
    return PPC_NAN;
  }
  return r;
}

inline double ni_sub(double a, double b) {
  const double r = a - b;
  if (std::isnan(r)) [[unlikely]] {
    if (std::isnan(a)) return make_quiet(a);
    if (std::isnan(b)) return make_quiet(b);
    return PPC_NAN;
  }
  return r;
}

inline double ni_mul(double a, double b) {
  const double r = a * b;
  if (std::isnan(r)) [[unlikely]] {
    if (std::isnan(a)) return make_quiet(a);
    if (std::isnan(b)) return make_quiet(b);
    return PPC_NAN;
  }
  return r;
}

inline double ni_div(double a, double b) {
  const double r = a / b;
  if (std::isnan(r)) [[unlikely]] {
    if (std::isnan(a)) return make_quiet(a);
    if (std::isnan(b)) return make_quiet(b);
    return PPC_NAN;
  }
  return r;
}

// (a * c) +/- b. For single precision, frC is rounded to 25 bits and the
// result is corrected so that it rounds to single only once (see Dolphin's
// NI_madd_msub for the full derivation).
template <bool sub, bool single>
inline double ni_madd_msub(double a, double c, double b) {
  double result;
  if constexpr (!single) {
    result = std::fma(a, c, sub ? -b : b);
  } else {
    const double c_round = force_25bit(c);
    const double b_sign = sub ? -b : b;
    result = std::fma(a, c_round, b_sign);

    const u64 result_bits = std::bit_cast<u64>(result);
    constexpr u64 D_MASK = 0x000000001fffffff;
    constexpr u64 EVEN_TIE = 0x0000000010000000;
    if ((result_bits & D_MASK) == EVEN_TIE) {
      // Exactly halfway between two singles: use the FMA's rounding error
      // (2Sum) to decide the direction a single rounding would have taken.
      const double a_prime = b_sign - result;
      const double b_prime = result + a_prime;
      const double delta_a = std::fma(a, c_round, a_prime);
      const double delta_b = b_sign - b_prime;
      const double error = delta_a + delta_b;
      if (error != 0.0) {
        if ((error > 0.0) == (result > 0.0))
          result = std::bit_cast<double>(result_bits + 1);
        else
          result = std::bit_cast<double>(result_bits - 1);
      }
    }
  }

  if (std::isnan(result)) [[unlikely]] {
    if (std::isnan(a)) return make_quiet(a);
    if (std::isnan(b)) return make_quiet(b);
    if (std::isnan(c)) return make_quiet(c);
    return PPC_NAN;
  }
  return result;
}

// Load/store conversions (PowerPC Programming Environments Manual; bit-exact).
inline u32 convert_to_single(u64 x) {
  const u32 exp = static_cast<u32>((x >> 52) & 0x7ff);
  if (exp > 896 || (x & ~DOUBLE_SIGN) == 0) {
    return static_cast<u32>(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  } else if (exp >= 874) {
    u32 t = static_cast<u32>(0x80000000 | ((x & DOUBLE_FRAC) >> 21));
    t = t >> (905 - exp);
    t |= static_cast<u32>((x >> 32) & 0x80000000);
    return t;
  } else {
    // Undefined by the manual; this matches hardware tests.
    return static_cast<u32>(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  }
}

// Used by psq_st: denormal singles flush to zero.
inline u32 convert_to_single_ftz(u64 x) {
  const u32 exp = static_cast<u32>((x >> 52) & 0x7ff);
  if (exp > 896 || (x & ~DOUBLE_SIGN) == 0)
    return static_cast<u32>(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  return static_cast<u32>((x >> 32) & 0x80000000);
}

inline u64 convert_to_double(u32 value) {
  const u64 x = value;
  u64 exp = (x >> 23) & 0xff;
  u64 frac = x & 0x007fffff;
  if (exp > 0 && exp < 255) {
    const u64 y = !(exp >> 7);
    const u64 z = y << 61 | y << 60 | y << 59;
    return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
  } else if (exp == 0 && frac != 0) {
    exp = 1023 - 126;
    do {
      frac <<= 1;
      exp -= 1;
    } while ((frac & 0x00800000) == 0);
    return ((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007fffff) << 29);
  } else {
    const u64 y = exp >> 7;
    const u64 z = y << 61 | y << 60 | y << 59;
    return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
  }
}

// Hardware-exact estimates used by fres/ps_res and frsqrte/ps_rsqrte (fp.cpp).
double approximate_reciprocal(double val);
double approximate_reciprocal_square_root(double val);

// Quantization scale tables for psq_l / psq_st (fp.cpp).
extern const float dequantize_table[64];
extern const float quantize_table[64];

}  // namespace ppc::fp
