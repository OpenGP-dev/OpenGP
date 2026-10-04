// SPDX-License-Identifier: GPL-3.0-or-later
//
// Estimate tables and quantization scales. Ported from the Dolphin emulator
// (Source/Core/Common/FloatUtils.cpp and Interpreter_LoadStorePaired.cpp),
// Copyright Dolphin Emulator Project, GPL-2.0-or-later.

#include "ppc/fp.h"

#include <array>

namespace ppc::fp {

namespace {

struct BaseAndDec {
  int base;
  int dec;
};

constexpr std::array<BaseAndDec, 32> frsqrte_expected = {{
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
}};

constexpr std::array<BaseAndDec, 32> fres_expected = {{
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
}};

}  // namespace

double approximate_reciprocal_square_root(double val) {
  s64 integral = std::bit_cast<s64>(val);
  s64 mantissa = integral & ((1LL << 52) - 1);
  const s64 sign = integral & static_cast<s64>(1ULL << 63);
  s64 exponent = integral & (0x7FFLL << 52);

  if (mantissa == 0 && exponent == 0) {
    return sign ? -std::numeric_limits<double>::infinity()
                : std::numeric_limits<double>::infinity();
  }
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) {
      if (sign) return std::numeric_limits<double>::quiet_NaN();
      return 0.0;
    }
    return make_quiet(val);
  }
  if (sign) return std::numeric_limits<double>::quiet_NaN();

  if (!exponent) {
    // "Normalize" denormal values.
    do {
      exponent -= 1LL << 52;
      mantissa <<= 1;
    } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }

  const s64 exponent_lsb = exponent & (1LL << 52);
  exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
  integral = sign | exponent;

  const int i = static_cast<int>((exponent_lsb | mantissa) >> 37);
  const auto& entry = frsqrte_expected[i / 2048];
  integral |= static_cast<s64>(entry.base + entry.dec * (i % 2048)) << 26;
  return std::bit_cast<double>(integral);
}

double approximate_reciprocal(double val) {
  s64 integral = std::bit_cast<s64>(val);
  const s64 mantissa = integral & ((1LL << 52) - 1);
  const s64 sign = integral & static_cast<s64>(1ULL << 63);
  s64 exponent = integral & (0x7FFLL << 52);

  if (mantissa == 0 && exponent == 0)
    return std::copysign(std::numeric_limits<double>::infinity(), val);
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) return std::copysign(0.0, val);
    return make_quiet(val);
  }
  if (exponent < (895LL << 52)) return std::copysign(std::numeric_limits<float>::max(), val);
  if (exponent >= (1149LL << 52)) return std::copysign(0.0, val);

  exponent = (0x7FDLL << 52) - exponent;
  const int i = static_cast<int>(mantissa >> 37);
  const auto& entry = fres_expected[i / 1024];
  integral = sign | exponent;
  integral |= static_cast<s64>(entry.base - (entry.dec * (i % 1024) + 1) / 2) << 29;
  return std::bit_cast<double>(integral);
}

// Scales indexed by the 6-bit signed GQR scale field: 0..31 divide by 2^n on
// load (multiply on store), 32..63 are the negative scales -32..-1.
#define P(n) static_cast<float>(1ULL << (n))
#define N(n) static_cast<float>(1.0 / static_cast<double>(1ULL << (n)))
const float dequantize_table[64] = {
    N(0),  N(1),  N(2),  N(3),  N(4),  N(5),  N(6),  N(7),  N(8),  N(9),  N(10), N(11), N(12),
    N(13), N(14), N(15), N(16), N(17), N(18), N(19), N(20), N(21), N(22), N(23), N(24), N(25),
    N(26), N(27), N(28), N(29), N(30), N(31), P(32), P(31), P(30), P(29), P(28), P(27), P(26),
    P(25), P(24), P(23), P(22), P(21), P(20), P(19), P(18), P(17), P(16), P(15), P(14), P(13),
    P(12), P(11), P(10), P(9),  P(8),  P(7),  P(6),  P(5),  P(4),  P(3),  P(2),  P(1),
};
const float quantize_table[64] = {
    P(0),  P(1),  P(2),  P(3),  P(4),  P(5),  P(6),  P(7),  P(8),  P(9),  P(10), P(11), P(12),
    P(13), P(14), P(15), P(16), P(17), P(18), P(19), P(20), P(21), P(22), P(23), P(24), P(25),
    P(26), P(27), P(28), P(29), P(30), P(31), N(32), N(31), N(30), N(29), N(28), N(27), N(26),
    N(25), N(24), N(23), N(22), N(21), N(20), N(19), N(18), N(17), N(16), N(15), N(14), N(13),
    N(12), N(11), N(10), N(9),  N(8),  N(7),  N(6),  N(5),  N(4),  N(3),  N(2),  N(1),
};
#undef P
#undef N

}  // namespace ppc::fp
