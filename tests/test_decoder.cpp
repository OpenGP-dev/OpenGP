// SPDX-License-Identifier: GPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <random>
#include <set>
#include <vector>

#include "gekko/isa.h"

using namespace gekko;

namespace {

s64 random_field_value(const FieldDesc& f, std::mt19937& rng) {
  if (f.kind == FieldKind::BranchTarget) {
    // Multiple of 4 within the signed range of the (scaled) field.
    const s64 span = s64{1} << (f.width - 1);
    std::uniform_int_distribution<s64> dist(-span, span - 1);
    return dist(rng) * 4;
  }
  if (f.is_signed) {
    const s64 span = s64{1} << (f.width - 1);
    std::uniform_int_distribution<s64> dist(-span, span - 1);
    return dist(rng);
  }
  std::uniform_int_distribution<s64> dist(0, (s64{1} << f.width) - 1);
  return dist(rng);
}

}  // namespace

TEST_CASE("every opcode round-trips through encode/decode") {
  std::mt19937 rng(1234);
  std::set<std::string> ids;
  for (const OpInfo& info : all_ops()) {
    const std::string mnemonic = info.mnemonic;
    CAPTURE(mnemonic);
    CHECK(ids.insert(info.id).second);  // unique identifiers

    const auto flds = sig_fields(info.sig);
    for (int iter = 0; iter < 256; ++iter) {
      std::vector<s64> values;
      for (const FieldDesc& f : flds) values.push_back(random_field_value(f, rng));
      const u32 raw = encode(info.op, values);
      CAPTURE(raw);
      REQUIRE(decode(raw) == info.op);
      for (std::size_t i = 0; i < flds.size(); ++i) {
        const std::string field = flds[i].name;
        CAPTURE(field);
        CHECK(field_get(flds[i], raw) == values[i]);
      }
    }
  }
}

TEST_CASE("compile-time field extraction matches the runtime descriptors") {
  std::mt19937 rng(99);
  for (int iter = 0; iter < 10000; ++iter) {
    const u32 raw = rng();
    CHECK(fields::rD::get(raw) == field_get(fields::rD::desc, raw));
    CHECK(fields::simm::get(raw) == field_get(fields::simm::desc, raw));
    CHECK(fields::spr::get(raw) == field_get(fields::spr::desc, raw));
    CHECK(fields::li::get(raw) == field_get(fields::li::desc, raw));
    CHECK(fields::bd::get(raw) == field_get(fields::bd::desc, raw));
    CHECK(fields::psq_d::get(raw) == field_get(fields::psq_d::desc, raw));
  }
}

TEST_CASE("known encodings") {
  CHECK(encode(Op::addi, {3, 3, 16}) == 0x38630010u);
  CHECK(encode(Op::stwu, {1, 1, -32}) == 0x9421FFE0u);
  CHECK(encode(Op::mfspr, {0, 8}) == 0x7C0802A6u);
  CHECK(encode(Op::bclr, {20, 0, 0}) == 0x4E800020u);
  CHECK(encode(Op::bcctr, {20, 0, 1}) == 0x4E800421u);
  CHECK(encode(Op::bc, {12, 2, 12, 0, 0}) == 0x4182000Cu);
  CHECK(encode(Op::or_, {5, 4, 4, 0}) == 0x7C852378u);
  CHECK(encode(Op::add, {3, 4, 5, 1, 0}) == 0x7C642E14u);
  CHECK(encode(Op::fmadds, {1, 2, 3, 4, 0}) == 0xEC2220FAu);
  CHECK(encode(Op::ps_add, {1, 2, 3, 0}) == 0x1022182Au);
  CHECK(encode(Op::psq_l, {1, 3, 0, 0, 0}) == 0xE0230000u);
  CHECK(encode(Op::mfspr, {3, 912}) == 0x7C70E2A6u);
}

TEST_CASE("disassembly") {
  const u32 pc = 0x80003100;
  CHECK(disasm(0x38630010, pc) == "addi r3, r3, 0x10");
  CHECK(disasm(0x3860FFFF, pc) == "li r3, -0x1");
  CHECK(disasm(0x3C60CC01, pc) == "lis r3, 0xcc01");
  CHECK(disasm(0x4E800020, pc) == "blr");
  CHECK(disasm(0x4E800420, pc) == "bctr");
  CHECK(disasm(0x4E800421, pc) == "bctrl");
  CHECK(disasm(0x7C0802A6, pc) == "mflr r0");
  CHECK(disasm(0x7C0903A6, pc) == "mtctr r0");
  CHECK(disasm(0x9421FFE0, pc) == "stwu r1, -0x20(r1)");
  CHECK(disasm(0x7C852378, pc) == "mr r5, r4");
  CHECK(disasm(0x60000000, pc) == "nop");
  CHECK(disasm(0x4800000C, pc) == "b 0x8000310C");
  CHECK(disasm(0x48000001, pc) == "bl 0x80003100");
  CHECK(disasm(0x4182000C, pc) == "beq 0x8000310C");
  CHECK(disasm(0x4200FFF8, pc) == "bdnz 0x800030F8");
  CHECK(disasm(0x7C642A15, pc) == "add. r3, r4, r5");
  CHECK(disasm(0x7C642E14, pc) == "addo r3, r4, r5");
  CHECK(disasm(0x5483103A, pc) == "slwi r3, r4, 2");
  CHECK(disasm(0x2C030000, pc) == "cmpwi r3, 0x0");
  CHECK(disasm(0xEC2220FA, pc) == "fmadds f1, f2, f3, f4");
  CHECK(disasm(0x1022182A, pc) == "ps_add f1, f2, f3");
  CHECK(disasm(0xE0230000, pc) == "psq_l f1, 0x0(r3), 0, 0");
  CHECK(disasm(0x7C70E2A6, pc) == "mfspr r3, GQR0");
  CHECK(disasm(0x00000000, pc) == ".long 0x00000000");
}

TEST_CASE("gpr_writes") {
  CHECK(gpr_writes(0x38630010, Op::addi) == (1u << 3));
  CHECK(gpr_writes(0x9421FFE0, Op::stwu) == (1u << 1));
  CHECK(gpr_writes(0x7C852378, Op::or_) == (1u << 5));
  CHECK(gpr_writes(encode(Op::lwzu, {4, 5, 8}), Op::lwzu) == ((1u << 4) | (1u << 5)));
  CHECK(gpr_writes(encode(Op::lmw, {29, 1, 8}), Op::lmw) == 0xE0000000u);
  CHECK(gpr_writes(encode(Op::stw, {4, 5, 8}), Op::stw) == 0u);
}
