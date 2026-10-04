// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instruction field extraction. Each field is a struct with a compile-time
// get() (used by the interpreter through SigDef<...>::args) and a runtime
// descriptor (used by the disassembler, the encoder and the recompiler).
#pragma once

#include <array>
#include <cstddef>
#include <tuple>

#include "gekko/types.h"

namespace gekko {

enum class FieldKind : u8 {
  Gpr,           // r0..r31
  Fpr,           // f0..f31
  Crf,           // cr0..cr7
  Crb,           // CR bit number 0..31
  Dec,           // plain unsigned decimal
  Hex,           // unsigned hex (masks)
  SImm,          // signed 16-bit immediate
  UImm,          // unsigned 16-bit immediate
  Disp,          // signed memory displacement
  Spr,           // SPR/TBR number
  BranchTarget,  // branch displacement (bytes)
  Flag,          // mnemonic suffix bit (Rc, OE, LK, AA); not printed as an operand
};

struct FieldDesc {
  const char* name;
  u8 shift;
  u8 width;
  bool is_signed;
  FieldKind kind;
};

namespace fields {

constexpr s32 sign_extend(u32 value, unsigned bits) {
  const u32 m = 1u << (bits - 1);
  return static_cast<s32>((value ^ m) - m);
}

#define GEKKO_FIELD_U(NAME, SHIFT, WIDTH, KIND)                                      \
  struct NAME {                                                                      \
    using type = u32;                                                                \
    static constexpr FieldDesc desc{#NAME, SHIFT, WIDTH, false, FieldKind::KIND};    \
    static constexpr u32 get(u32 i) { return (i >> SHIFT) & ((1u << WIDTH) - 1); }   \
  };
#define GEKKO_FIELD_S(NAME, SHIFT, WIDTH, KIND)                                      \
  struct NAME {                                                                      \
    using type = s32;                                                                \
    static constexpr FieldDesc desc{#NAME, SHIFT, WIDTH, true, FieldKind::KIND};     \
    static constexpr s32 get(u32 i) {                                                \
      return sign_extend((i >> SHIFT) & ((1u << WIDTH) - 1), WIDTH);                 \
    }                                                                                \
  };
#define GEKKO_FIELD(NAME, SHIFT, WIDTH, SIGN, KIND) GEKKO_FIELD_##SIGN(NAME, SHIFT, WIDTH, KIND)
#include "gekko/fields.inc"
#undef GEKKO_FIELD
#undef GEKKO_FIELD_U
#undef GEKKO_FIELD_S

// SPR / TBR number: the two 5-bit halves are stored swapped.
struct spr {
  using type = u32;
  static constexpr FieldDesc desc{"spr", 11, 10, false, FieldKind::Spr};
  static constexpr u32 get(u32 i) { return ((i >> 16) & 31) | (((i >> 11) & 31) << 5); }
};

// I-form branch displacement: 24 bits, scaled by 4.
struct li {
  using type = s32;
  static constexpr FieldDesc desc{"li", 2, 24, true, FieldKind::BranchTarget};
  static constexpr s32 get(u32 i) { return sign_extend(i & 0x03FFFFFC, 26); }
};

// B-form branch displacement: 14 bits, scaled by 4.
struct bd {
  using type = s32;
  static constexpr FieldDesc desc{"bd", 2, 14, true, FieldKind::BranchTarget};
  static constexpr s32 get(u32 i) { return sign_extend(i & 0xFFFC, 16); }
};

}  // namespace fields

// Runtime access by descriptor (used by the tools, not on hot paths).
s64 field_get(const FieldDesc& f, u32 raw);
u32 field_put(const FieldDesc& f, s64 value);  // returns the value's bits in instruction position

// An operand signature: an ordered list of fields.
template <typename... F>
struct SigDef {
  static constexpr std::size_t count = sizeof...(F);
  using Tuple = std::tuple<typename F::type...>;
  static constexpr Tuple args([[maybe_unused]] u32 raw) { return Tuple{F::get(raw)...}; }
  static constexpr std::array<FieldDesc, sizeof...(F)> descs{F::desc...};
};

namespace sigs {
#define GEKKO_SIG(NAME, ...)                       \
  namespace sig_detail_##NAME {                    \
  using namespace ::gekko::fields;                 \
  using type = SigDef<__VA_ARGS__>;                \
  }                                                \
  using NAME = sig_detail_##NAME::type;
#include "gekko/sigs.inc"
#undef GEKKO_SIG
}  // namespace sigs

}  // namespace gekko
