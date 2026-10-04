// SPDX-License-Identifier: GPL-3.0-or-later
//
// Known-answer tests for instruction semantics (runtime/ppc/ops.h). Expected
// values are worked out by hand from the PowerPC manuals, or come from
// Dolphin's hardware-verified cases where noted.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <bit>
#include <cmath>
#include <cstring>

#include "gekko/isa.h"
#include "ppc/interpreter.h"
#include "ppc/memory.h"
#include "ppc/ops.h"

using namespace ppc;
using gekko::Op;
using gekko::encode;

namespace {

GuestMemory& memory() {
  static GuestMemory mem;
  static bool ok = [] {
    std::string err;
    const bool r = mem.init(MEM1_SIZE_GAMECUBE, &err);
    if (!r) MESSAGE(err);
    return r;
  }();
  REQUIRE(ok);
  return mem;
}

struct Fixture {
  Context c{};
  u8* base = memory().base();
  Fixture() {
    reset_context(c);
    reset_hooks();
    std::memset(base + MEM1_CACHED, 0, 0x10000);
  }
};

u64 bits(double d) { return std::bit_cast<u64>(d); }
double single_bits(u32 b) { return static_cast<double>(std::bit_cast<float>(b)); }

}  // namespace

TEST_CASE("rotate masks") {
  CHECK(rotate_mask(0, 31) == 0xFFFFFFFFu);
  CHECK(rotate_mask(0, 0) == 0x80000000u);
  CHECK(rotate_mask(31, 31) == 0x00000001u);
  CHECK(rotate_mask(5, 2) == 0xE7FFFFFFu);
  CHECK(rotate_mask(16, 15) == 0xFFFFFFFFu);
  CHECK(rotate_mask(24, 31) == 0x000000FFu);
}

TEST_CASE_FIXTURE(Fixture, "rotates and shifts") {
  c.gpr[4] = 0x12345678;
  op::rlwinm(c, 3, 4, 8, 24, 31, 0);
  CHECK(c.gpr[3] == 0x12u);

  c.gpr[3] = 0xFFFFFFFF;
  op::rlwimi(c, 3, 4, 0, 16, 23, 0);
  CHECK(c.gpr[3] == 0xFFFF56FFu);

  c.gpr[5] = 36;  // rotate amount uses the low 5 bits
  op::rlwnm(c, 3, 4, 5, 0, 31, 0);
  CHECK(c.gpr[3] == 0x23456781u);

  c.gpr[4] = 0xFFFFFFF1;  // -15
  op::srawi(c, 3, 4, 2, 0);
  CHECK(c.gpr[3] == 0xFFFFFFFCu);
  CHECK(c.xer_ca == 1);
  c.gpr[4] = 0xFFFFFFF0;  // -16: no one-bits shifted out
  op::srawi(c, 3, 4, 2, 0);
  CHECK(c.gpr[3] == 0xFFFFFFFCu);
  CHECK(c.xer_ca == 0);
  c.gpr[4] = 15;
  op::srawi(c, 3, 4, 2, 0);
  CHECK(c.gpr[3] == 3u);
  CHECK(c.xer_ca == 0);

  c.gpr[4] = 0xFFFFFFFF;
  c.gpr[5] = 40;
  op::sraw(c, 3, 4, 5, 0);
  CHECK(c.gpr[3] == 0xFFFFFFFFu);
  CHECK(c.xer_ca == 1);
  c.gpr[4] = 5;
  c.gpr[5] = 33;
  op::sraw(c, 3, 4, 5, 0);
  CHECK(c.gpr[3] == 0u);
  CHECK(c.xer_ca == 0);

  c.gpr[4] = 0xFFFFFFFF;
  c.gpr[5] = 32;
  op::slw(c, 3, 4, 5, 0);
  CHECK(c.gpr[3] == 0u);
  c.gpr[4] = 0x80000000;
  c.gpr[5] = 31;
  op::srw(c, 3, 4, 5, 0);
  CHECK(c.gpr[3] == 1u);

  c.gpr[4] = 0;
  op::cntlzw(c, 3, 4, 0);
  CHECK(c.gpr[3] == 32u);
  c.gpr[4] = 1;
  op::cntlzw(c, 3, 4, 0);
  CHECK(c.gpr[3] == 31u);

  c.gpr[4] = 0x80;
  op::extsb(c, 3, 4, 0);
  CHECK(c.gpr[3] == 0xFFFFFF80u);
  c.gpr[4] = 0x8000;
  op::extsh(c, 3, 4, 0);
  CHECK(c.gpr[3] == 0xFFFF8000u);
}

TEST_CASE_FIXTURE(Fixture, "carries, overflow and CR0") {
  // 64-bit add: 0x00000001_FFFFFFFF + 1
  c.gpr[3] = 0xFFFFFFFF;
  c.gpr[4] = 1;
  c.gpr[5] = 1;
  c.gpr[6] = 0;
  op::addc(c, 7, 3, 5, 0, 0);
  op::adde(c, 8, 4, 6, 0, 0);
  CHECK(c.gpr[7] == 0u);
  CHECK(c.gpr[8] == 2u);

  c.gpr[3] = 5;
  c.gpr[4] = 3;
  op::subfc(c, 5, 3, 4, 0, 0);  // 3 - 5
  CHECK(c.gpr[5] == 0xFFFFFFFEu);
  CHECK(c.xer_ca == 0);
  op::subfc(c, 5, 4, 3, 0, 0);  // 5 - 3
  CHECK(c.gpr[5] == 2u);
  CHECK(c.xer_ca == 1);

  c.gpr[3] = 10;
  op::subfic(c, 4, 3, 3);  // 3 - 10
  CHECK(c.gpr[4] == 0xFFFFFFF9u);
  CHECK(c.xer_ca == 0);

  c.gpr[3] = 0x7FFFFFFF;
  c.gpr[4] = 1;
  op::add(c, 5, 3, 4, 1, 1);
  CHECK(c.gpr[5] == 0x80000000u);
  CHECK(c.xer_ov == 1);
  CHECK(c.xer_so == 1);
  CHECK(cr_field(c, 0) == 0x9u);  // LT | SO
  c.gpr[3] = 1;
  op::add(c, 5, 3, 4, 1, 1);
  CHECK(c.xer_ov == 0);
  CHECK(c.xer_so == 1);  // sticky
  CHECK(cr_field(c, 0) == 0x5u);  // GT | SO

  c.gpr[3] = 0x80000000;
  op::neg(c, 4, 3, 1, 0);
  CHECK(c.gpr[4] == 0x80000000u);
  CHECK(c.xer_ov == 1);

  c.gpr[3] = 0xFFFFFFFF;
  op::addic_rc(c, 4, 3, 1);
  CHECK(c.gpr[4] == 0u);
  CHECK(c.xer_ca == 1);
  CHECK((cr_field(c, 0) & 0xE) == 0x2u);  // EQ
}

TEST_CASE_FIXTURE(Fixture, "multiply and divide edge cases") {
  c.gpr[3] = 7;
  c.gpr[4] = static_cast<u32>(-2);
  op::divw(c, 5, 3, 4, 0, 0);
  CHECK(c.gpr[5] == static_cast<u32>(-3));

  c.gpr[4] = 0;
  c.gpr[3] = 5;
  op::divw(c, 5, 3, 4, 1, 0);
  CHECK(c.gpr[5] == 0u);
  CHECK(c.xer_ov == 1);
  c.gpr[3] = static_cast<u32>(-5);
  op::divw(c, 5, 3, 4, 0, 0);
  CHECK(c.gpr[5] == 0xFFFFFFFFu);
  c.gpr[3] = 0x80000000;
  c.gpr[4] = 0xFFFFFFFF;
  op::divw(c, 5, 3, 4, 1, 0);
  CHECK(c.gpr[5] == 0xFFFFFFFFu);
  CHECK(c.xer_ov == 1);

  c.gpr[3] = 9;
  c.gpr[4] = 0;
  op::divwu(c, 5, 3, 4, 1, 0);
  CHECK(c.gpr[5] == 0u);
  CHECK(c.xer_ov == 1);

  c.gpr[3] = 0x80000000;
  c.gpr[4] = 2;
  op::mulhw(c, 5, 3, 4, 0);
  CHECK(c.gpr[5] == 0xFFFFFFFFu);
  op::mulhwu(c, 5, 3, 4, 0);
  CHECK(c.gpr[5] == 1u);

  c.gpr[3] = 0x10000;
  c.gpr[4] = 0x10000;
  c.xer_so = 0;
  op::mullw(c, 5, 3, 4, 1, 0);
  CHECK(c.gpr[5] == 0u);
  CHECK(c.xer_ov == 1);

  c.gpr[3] = static_cast<u32>(-3);
  op::mulli(c, 4, 3, 1000);
  CHECK(c.gpr[4] == static_cast<u32>(-3000));
}

TEST_CASE_FIXTURE(Fixture, "compares and condition register") {
  c.gpr[3] = 0xFFFFFFFF;
  op::cmpi(c, 1, 3, -1);
  CHECK(cr_field(c, 1) == 2u);
  op::cmpli(c, 2, 3, 1);
  CHECK(cr_field(c, 2) == 4u);
  op::cmpi(c, 3, 3, 1);
  CHECK(cr_field(c, 3) == 8u);

  c.cr = 0;
  c.gpr[4] = 0x12345678;
  op::mtcrf(c, 0x81, 4);
  CHECK(c.cr == 0x10000008u);
  op::mfcr(c, 5);
  CHECK(c.gpr[5] == 0x10000008u);

  c.cr = 0;
  set_cr_bit(c, 0, 1);
  set_cr_bit(c, 5, 1);
  op::crand(c, 10, 0, 5);
  op::crxor(c, 11, 0, 5);
  op::cror(c, 12, 1, 5);
  op::crnor(c, 13, 1, 2);
  CHECK(cr_bit(c, 10) == 1u);
  CHECK(cr_bit(c, 11) == 0u);
  CHECK(cr_bit(c, 12) == 1u);
  CHECK(cr_bit(c, 13) == 1u);

  op::mcrf(c, 7, 0);
  CHECK(cr_field(c, 7) == cr_field(c, 0));

  c.xer_so = 1;
  c.xer_ov = 0;
  c.xer_ca = 1;
  op::mcrxr(c, 4);
  CHECK(cr_field(c, 4) == 0xAu);
  CHECK(get_xer(c) == 0u);

  // bdnz: decrement CTR, branch while non-zero.
  c.ctr = 2;
  CHECK(bc_cond(c, 16, 0));
  CHECK(c.ctr == 1u);
  CHECK_FALSE(bc_cond(c, 16, 0));
  CHECK(c.ctr == 0u);
}

TEST_CASE_FIXTURE(Fixture, "integer loads and stores, MEM1 mirrors") {
  const u32 a = 0x80001000;
  c.gpr[3] = a;
  c.gpr[4] = 0x12345678;
  op::stw<Mem::Ram>(c, base, 4, 3, 0);
  CHECK(base[a] == 0x12);
  CHECK(base[a + 3] == 0x78);
  // The uncached mirror sees the same bytes.
  CHECK(load_be<u32>(base + (a | 0x40000000)) == 0x12345678u);

  op::lhz<Mem::Ram>(c, base, 5, 3, 2);
  CHECK(c.gpr[5] == 0x5678u);
  c.gpr[4] = 0x8000;
  op::sth<Mem::Ram>(c, base, 4, 3, 4);
  op::lha<Mem::Ram>(c, base, 5, 3, 4);
  CHECK(c.gpr[5] == 0xFFFF8000u);
  c.gpr[6] = 0;
  op::lwbrx<Mem::Checked>(c, base, 5, 3, 6);
  CHECK(c.gpr[5] == 0x78563412u);

  op::stwu<Mem::Ram>(c, base, 4, 3, 0x10);
  CHECK(c.gpr[3] == a + 0x10);

  c.gpr[3] = a;
  c.gpr[29] = 1;
  c.gpr[30] = 2;
  c.gpr[31] = 3;
  op::stmw<Mem::Ram>(c, base, 29, 3, 0x20);
  c.gpr[29] = c.gpr[30] = c.gpr[31] = 0;
  op::lmw<Mem::Ram>(c, base, 29, 3, 0x20);
  CHECK(c.gpr[29] == 1u);
  CHECK(c.gpr[30] == 2u);
  CHECK(c.gpr[31] == 3u);

  std::memcpy(base + a + 0x40, "ABCDEF", 6);
  c.gpr[3] = a + 0x40;
  op::lswi<Mem::Ram>(c, base, 5, 3, 6);
  CHECK(c.gpr[5] == 0x41424344u);
  CHECK(c.gpr[6] == 0x45460000u);
  c.gpr[3] = a + 0x50;
  op::stswi<Mem::Ram>(c, base, 5, 3, 6);
  CHECK(std::memcmp(base + a + 0x50, "ABCDEF", 6) == 0);

  // lwarx / stwcx.
  c.gpr[3] = a;
  c.gpr[4] = 0;
  c.gpr[5] = 0xCAFEBABE;
  op::lwarx<Mem::Ram>(c, base, 6, 3, 4);
  op::stwcx_rc<Mem::Ram>(c, base, 5, 3, 4);
  CHECK((cr_field(c, 0) & 2) == 2u);
  CHECK(load_be<u32>(base + a) == 0xCAFEBABEu);
  op::stwcx_rc<Mem::Ram>(c, base, 4, 3, 4);  // reservation gone
  CHECK((cr_field(c, 0) & 2) == 0u);
  CHECK(load_be<u32>(base + a) == 0xCAFEBABEu);

  // dcbz clears the aligned 32-byte line.
  std::memset(base + a + 0x60, 0xFF, 64);
  c.gpr[3] = a + 0x65;
  op::dcbz<Mem::Ram>(c, base, 3, 4);
  CHECK(base[a + 0x60] == 0);
  CHECK(base[a + 0x7F] == 0);
  CHECK(base[a + 0x80] == 0xFF);
}

TEST_CASE("single/double conversions (bit-exact)") {
  CHECK(fp::convert_to_double(0x3F800000) == bits(1.0));
  CHECK(fp::convert_to_double(0x00000001) == 0x36A0000000000000ULL);  // smallest denormal
  CHECK(fp::convert_to_single(0x36A0000000000000ULL) == 0x00000001u);
  CHECK(fp::convert_to_double(0x7F800001) == 0x7FF0000020000000ULL);  // SNaN stays signalling
  CHECK(fp::convert_to_single(bits(-2.5)) == 0xC0200000u);
  CHECK(fp::convert_to_single_ftz(0x36A0000000000000ULL) == 0u);
  CHECK(fp::force_25bit(1.0 + std::ldexp(1.0, -25)) == 1.0 + std::ldexp(1.0, -24));
  CHECK(fp::force_25bit(1.0 + std::ldexp(1.0, -24)) == 1.0 + std::ldexp(1.0, -24));
}

TEST_CASE_FIXTURE(Fixture, "floating-point loads and stores") {
  const u32 a = 0x80002000;
  c.gpr[3] = a;
  store_be<u32>(base + a, 0x3F800000);
  c.fpr[1].ps1 = bits(7.0);
  op::lfs<Mem::Ram>(c, base, 1, 3, 0);
  CHECK(c.fpr[1].d0() == 1.0);
  CHECK(c.fpr[1].d1() == 1.0);  // lfs fills both lanes

  store_be<u64>(base + a + 8, bits(1.5));
  c.fpr[2].ps1 = bits(7.0);
  op::lfd<Mem::Ram>(c, base, 2, 3, 8);
  CHECK(c.fpr[2].d0() == 1.5);
  CHECK(c.fpr[2].d1() == 7.0);  // lfd leaves ps1 alone

  c.fpr[4].set0(-2.5);
  op::stfs<Mem::Ram>(c, base, 4, 3, 0x10);
  CHECK(load_be<u32>(base + a + 0x10) == 0xC0200000u);
  c.fpr[4].ps0 = 0x0123456789ABCDEFULL;
  op::stfd<Mem::Ram>(c, base, 4, 3, 0x18);
  CHECK(load_be<u64>(base + a + 0x18) == 0x0123456789ABCDEFULL);
  c.gpr[5] = 0x20;
  op::stfiwx<Mem::Ram>(c, base, 4, 3, 5);
  CHECK(load_be<u32>(base + a + 0x20) == 0x89ABCDEFu);
}

TEST_CASE_FIXTURE(Fixture, "FP arithmetic and NaN handling") {
  c.fpr[1].set0(1.0);
  c.fpr[2].set0(3.0);
  op::fdiv(c, 3, 1, 2, 0);
  CHECK(c.fpr[3].d0() == 1.0 / 3.0);
  op::fdivs(c, 4, 1, 2, 0);
  CHECK(c.fpr[4].d0() == static_cast<double>(1.0f / 3.0f));
  CHECK(c.fpr[4].d1() == static_cast<double>(1.0f / 3.0f));  // single results fill both lanes

  // Single-precision FMA rounds once (Dolphin hardware test, Mario Strikers Charged).
  c.fpr[1].set0(single_bits(0x42480000));
  c.fpr[3].set0(single_bits(0xbc88cc38));
  c.fpr[2].set0(single_bits(0x1b1c72a0));
  op::fmadds(c, 5, 1, 3, 2, 0);
  CHECK(std::bit_cast<u32>(static_cast<float>(c.fpr[5].d0())) == 0xbf55bf17u);

  // NaN operands propagate in the order frA, frB, frC and are quietened.
  const u64 nan_b = 0x7FF0000000000B0BULL;  // signalling
  const u64 nan_c = 0x7FF8000000000C0CULL;
  c.fpr[1].set0(2.0);
  c.fpr[2].ps0 = nan_b;
  c.fpr[3].ps0 = nan_c;
  op::fmadd(c, 6, 1, 3, 2, 0);
  CHECK(c.fpr[6].ps0 == (nan_b | fp::DOUBLE_QBIT));
  op::fmul(c, 6, 1, 3, 0);
  CHECK(c.fpr[6].ps0 == nan_c);

  // An invalid operation produces the PowerPC default NaN (positive).
  c.fpr[1].set0(INFINITY);
  c.fpr[2].set0(INFINITY);
  op::fsub(c, 7, 1, 2, 0);
  CHECK(c.fpr[7].ps0 == 0x7FF8000000000000ULL);

  // fnmadd does not negate NaN results.
  c.fpr[1].set0(INFINITY);
  c.fpr[2].set0(-INFINITY);
  c.fpr[3].set0(1.0);
  op::fnmadd(c, 7, 1, 3, 2, 0);
  CHECK(c.fpr[7].ps0 == 0x7FF8000000000000ULL);

  c.fpr[1].set0(-0.0);
  c.fpr[2].set0(10.0);
  c.fpr[3].set0(20.0);
  op::fsel(c, 8, 1, 3, 2, 0);
  CHECK(c.fpr[8].d0() == 20.0);  // -0.0 >= 0

  c.fpr[1].set0(1.0 / 3.0);
  op::frsp(c, 9, 1, 0);
  CHECK(c.fpr[9].d0() == static_cast<double>(1.0f / 3.0f));
  CHECK(c.fpr[9].d1() == static_cast<double>(1.0f / 3.0f));

  c.fpr[1].set0(1.0);
  c.fpr[2].set0(2.0);
  op::fcmpu(c, 3, 1, 2);
  CHECK(cr_field(c, 3) == 8u);
  CHECK((c.fpscr & fp::FPSCR_FPCC) == 0x8000u);
  c.fpr[2].ps0 = 0x7FF8000000000000ULL;
  op::fcmpu(c, 3, 1, 2);
  CHECK(cr_field(c, 3) == 1u);
}

TEST_CASE_FIXTURE(Fixture, "estimates (hardware tables)") {
  CHECK(fp::approximate_reciprocal(2.0) == 0.49993896484375);
  CHECK(fp::approximate_reciprocal_square_root(1.0) == 0.99981689453125);
  CHECK(fp::approximate_reciprocal(0.0) == INFINITY);
  CHECK(fp::approximate_reciprocal(-0.0) == -INFINITY);
  CHECK(fp::approximate_reciprocal(INFINITY) == 0.0);
  CHECK(std::isnan(fp::approximate_reciprocal_square_root(-1.0)));
  CHECK(fp::approximate_reciprocal_square_root(INFINITY) == 0.0);

  // Estimates are within the architected error bounds.
  for (double v : {0.1, 0.5, 3.0, 17.25, 1000.0, 123456.0}) {
    CHECK(std::fabs(fp::approximate_reciprocal(v) * v - 1.0) < 1.0 / 4096);
    CHECK(std::fabs(fp::approximate_reciprocal_square_root(v) * std::sqrt(v) - 1.0) < 1.0 / 4096);
  }

  c.fpr[1].set0(2.0);
  op::fres(c, 2, 1, 0);
  CHECK(c.fpr[2].d0() == 0.49993896484375);
  CHECK(c.fpr[2].d1() == 0.49993896484375);
}

TEST_CASE_FIXTURE(Fixture, "float to integer conversion") {
  auto conv = [&](double v, bool z) {
    c.fpr[1].set0(v);
    if (z) op::fctiwz(c, 2, 1, 0);
    else op::fctiw(c, 2, 1, 0);
    return c.fpr[2].ps0;
  };
  CHECK(conv(2.7, true) == 0xFFF8000000000002ULL);
  CHECK(conv(-2.7, true) == 0xFFF80000FFFFFFFEULL);
  CHECK(conv(3e10, true) == 0xFFF800007FFFFFFFULL);
  CHECK(conv(-3e10, true) == 0xFFF8000080000000ULL);
  CHECK(conv(NAN, true) == 0xFFF8000080000000ULL);
  CHECK(conv(-0.0, true) == 0xFFF8000100000000ULL);
  c.fpscr = 0;  // round to nearest even
  CHECK(conv(2.5, false) == 0xFFF8000000000002ULL);
  CHECK(conv(3.5, false) == 0xFFF8000000000004ULL);
  c.fpscr = 3;  // toward -inf
  CHECK(conv(2.5, false) == 0xFFF8000000000002ULL);
  CHECK(conv(-2.5, false) == 0xFFF80000FFFFFFFDULL);
}

TEST_CASE_FIXTURE(Fixture, "FPSCR instructions") {
  c.fpscr = 0;
  op::mtfsfi(c, 7, 3, 0);  // RN = 3
  CHECK(c.fpscr == 3u);
  op::mtfsb1(c, 29, 0);  // NI
  CHECK(c.fpscr == 7u);
  op::mtfsb1(c, 3, 0);  // OX: an exception bit also sets FX
  CHECK((c.fpscr & fp::FPSCR_OX) != 0);
  CHECK((c.fpscr & fp::FPSCR_FX) != 0);
  op::mcrfs(c, 2, 0);  // copies FX FEX VX OX and clears the exception bits
  CHECK(cr_field(c, 2) == 0x9u);
  CHECK((c.fpscr & (fp::FPSCR_FX | fp::FPSCR_OX)) == 0u);
  op::mffs(c, 1, 0);
  CHECK(c.fpr[1].ps0 == (0xFFF8000000000000ULL | c.fpscr));
  c.fpr[2].ps0 = 0x00000000000000F0ULL;
  op::mtfsf(c, 0x01, 2, 0);  // only field 7
  CHECK((c.fpscr & 0xF) == 0u);
}

TEST_CASE_FIXTURE(Fixture, "paired singles") {
  c.fpr[1].set_both(1.0, 2.0);
  c.fpr[2].set_both(3.0, 4.0);
  c.fpr[3].set_both(5.0, 6.0);

  op::ps_add(c, 4, 1, 2, 0);
  CHECK(c.fpr[4].d0() == 4.0);
  CHECK(c.fpr[4].d1() == 6.0);
  op::ps_sum0(c, 4, 1, 3, 2, 0);  // (a0 + b1, c1)
  CHECK(c.fpr[4].d0() == 5.0);
  CHECK(c.fpr[4].d1() == 6.0);
  op::ps_sum1(c, 4, 1, 3, 2, 0);  // (c0, a0 + b1)
  CHECK(c.fpr[4].d0() == 5.0);
  CHECK(c.fpr[4].d1() == 5.0);
  op::ps_muls0(c, 4, 1, 3, 0);  // a * c0
  CHECK(c.fpr[4].d0() == 5.0);
  CHECK(c.fpr[4].d1() == 10.0);
  op::ps_madds1(c, 4, 1, 3, 2, 0);  // a * c1 + b
  CHECK(c.fpr[4].d0() == 9.0);
  CHECK(c.fpr[4].d1() == 16.0);
  op::ps_merge10(c, 4, 1, 2, 0);
  CHECK(c.fpr[4].d0() == 2.0);
  CHECK(c.fpr[4].d1() == 3.0);
  op::ps_neg(c, 4, 1, 0);
  CHECK(c.fpr[4].d0() == -1.0);
  CHECK(c.fpr[4].d1() == -2.0);
  c.fpr[5].set_both(-1.0, 1.0);
  op::ps_sel(c, 4, 5, 2, 3, 0);  // a >= 0 ? c : b
  CHECK(c.fpr[4].d0() == 5.0);
  CHECK(c.fpr[4].d1() == 4.0);
  op::ps_cmpu1(c, 6, 1, 2);
  CHECK(cr_field(c, 6) == 8u);
}

TEST_CASE_FIXTURE(Fixture, "quantized loads and stores") {
  const u32 a = 0x80003000;
  c.gpr[3] = a;

  base[a] = 0x80;
  base[a + 1] = 0xFF;
  c.spr[spr::GQR0 + 2] = 4u << 16;  // load u8, scale 0
  op::psq_l<Mem::Ram>(c, base, 1, 3, 0, 0, 2);
  CHECK(c.fpr[1].d0() == 128.0);
  CHECK(c.fpr[1].d1() == 255.0);

  base[a + 1] = 0x02;
  c.spr[spr::GQR0 + 3] = (1u << 24) | (6u << 16);  // load s8, scale 1 (divide by 2)
  op::psq_l<Mem::Ram>(c, base, 1, 3, 0, 0, 3);
  CHECK(c.fpr[1].d0() == -64.0);
  CHECK(c.fpr[1].d1() == 1.0);

  store_be<u16>(base + a, 3);
  c.spr[spr::GQR0 + 4] = (63u << 24) | (7u << 16);  // load s16, scale -1 (multiply by 2)
  op::psq_l<Mem::Ram>(c, base, 1, 3, 0, 1, 4);    // W=1: ps1 = 1.0
  CHECK(c.fpr[1].d0() == 6.0);
  CHECK(c.fpr[1].d1() == 1.0);

  c.spr[spr::GQR0 + 5] = (8u << 8) | 7u;  // store s16, scale 8
  c.fpr[2].set_both(1.5, -1.0);
  op::psq_st<Mem::Ram>(c, base, 2, 3, 0x10, 0, 5);
  CHECK(load_be<u32>(base + a + 0x10) == 0x0180FF00u);

  c.spr[spr::GQR0 + 6] = 4u;  // store u8, scale 0: clamps
  c.fpr[2].set_both(300.0, -5.0);
  op::psq_st<Mem::Ram>(c, base, 2, 3, 0x20, 0, 6);
  CHECK(base[a + 0x20] == 255);
  CHECK(base[a + 0x21] == 0);

  c.spr[spr::GQR0 + 7] = 0;  // float
  c.fpr[2].set_both(1.0, -2.5);
  c.gpr[4] = 0x30;
  op::psq_stux<Mem::Ram>(c, base, 2, 3, 4, 0, 7);
  CHECK(c.gpr[3] == a + 0x30);
  CHECK(load_be<u32>(base + a + 0x30) == 0x3F800000u);
  CHECK(load_be<u32>(base + a + 0x34) == 0xC0200000u);
}

TEST_CASE_FIXTURE(Fixture, "locked cache DMA") {
  const u32 src = 0x80004000;
  for (u32 i = 0; i < 64; ++i) base[src + i] = static_cast<u8>(i + 1);
  c.gpr[3] = src & 0x1FFFFFFF;  // DMA_U: memory address, length high bits 0
  op::mtspr(c, base, spr::DMAU, 3);
  c.gpr[3] = LOCKED_CACHE_BASE | 0x10 | (2u << 2) | 0x2;  // load, 2 lines, trigger
  op::mtspr(c, base, spr::DMAL, 3);
  CHECK(std::memcmp(base + LOCKED_CACHE_BASE, base + src, 64) == 0);
  CHECK((c.spr[spr::DMAL] & 2) == 0u);
}

TEST_CASE_FIXTURE(Fixture, "gather pipe and MMIO routing") {
  g_default_gather_pipe.clear();
  c.gpr[3] = GATHER_PIPE_BASE;
  c.gpr[4] = 0x11223344;
  op::stw<Mem::GatherPipe>(c, base, 4, 3, 0);
  op::stb<Mem::Checked>(c, base, 4, 3, 0);
  c.fpr[1].set0(1.0);
  op::stfd<Mem::Checked>(c, base, 1, 3, 0);
  const std::vector<u8> expected = {0x11, 0x22, 0x33, 0x44, 0x44, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0};
  CHECK(g_default_gather_pipe == expected);

  g_hooks.mmio_read = [](u32 addr, u32 size) -> u32 { return (addr & 0xFFFF) + size; };
  c.gpr[3] = 0xCC003000;
  op::lhz<Mem::Checked>(c, base, 5, 3, 4);
  CHECK(c.gpr[5] == 0x3006u);
}

TEST_CASE_FIXTURE(Fixture, "interpreter runs a small function") {
  // r3 = sum(1..10) using a CTR loop, then return.
  const u32 fn = 0x80005000;
  const u32 prog[] = {
      encode(Op::addi, {3, 0, 0}),          // li r3, 0
      encode(Op::addi, {4, 0, 10}),         // li r4, 10
      encode(Op::mtspr, {9, 4}),            // mtctr r4
      encode(Op::add, {3, 3, 4, 0, 0}),     // loop: add r3, r3, r4
      encode(Op::addi, {4, 4, -1}),         // subi r4, r4, 1
      encode(Op::bc, {16, 0, -8, 0, 0}),    // bdnz loop
      encode(Op::bclr, {20, 0, 0}),         // blr
  };
  for (u32 i = 0; i < std::size(prog); ++i) store_be<u32>(base + fn + 4 * i, prog[i]);
  c.lr = 0x80000100;  // return address sentinel
  c.gpr[1] = 0x80100000;
  interpret(c, base, fn);
  CHECK(c.gpr[3] == 55u);
  CHECK(c.ctr == 0u);
}
