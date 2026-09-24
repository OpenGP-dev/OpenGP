// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gekko instruction set: opcode enumeration, decoder, disassembler, encoder.
#pragma once

#include <initializer_list>
#include <span>
#include <string>

#include "gekko/fields.h"
#include "gekko/types.h"

namespace gekko {

enum class Op : u16 {
  Invalid = 0,
#define GEKKO_OP(id, mn, opcd, xo, kind, sig, cc, flags) id,
#include "gekko/ops.inc"
#undef GEKKO_OP
  Count,
};

enum class Sig : u8 {
#define GEKKO_SIG(name, ...) name,
#include "gekko/sigs.inc"
#undef GEKKO_SIG
};

enum class XoKind : u8 { PRI, X10, X9, X5, X6 };
enum class CallConv : u8 { CTX, MEM, BASE, PC, BRANCH };

enum OpFlags : u32 {
  F_LOAD = 1u << 0,
  F_STORE = 1u << 1,
  F_UPD = 1u << 2,   // writes the effective address back to rA
  F_FP = 1u << 3,    // reads or writes FPRs
  F_PRIV = 1u << 4,  // supervisor-level instruction
};

struct OpInfo {
  Op op;
  const char* id;        // C++ identifier, e.g. "and_"
  const char* mnemonic;  // assembler mnemonic, e.g. "and"
  u8 opcd;
  u16 xo;
  XoKind kind;
  Sig sig;
  CallConv cc;
  u32 flags;
};

// Static information about an opcode. op_info(Op::Invalid) is valid and describes nothing.
const OpInfo& op_info(Op op);

// All real opcodes (excludes Op::Invalid).
std::span<const OpInfo> all_ops();

// Field list of a signature, in argument order.
std::span<const FieldDesc> sig_fields(Sig sig);
const char* sig_name(Sig sig);

// Decodes a raw big-endian-ordered instruction word. Returns Op::Invalid for
// encodings that are not part of the Gekko ISA.
Op decode(u32 raw);

// Disassembles one instruction. `pc` is used to resolve relative branch targets.
// Common simplified mnemonics (li, mr, blr, beq, ...) are used where applicable.
std::string disasm(u32 raw, u32 pc);

// Builds an instruction word: primary/extended opcode bits for `op` plus the
// given field values in the order of the op's signature. Branch displacements
// are given in bytes. Throws std::invalid_argument on a wrong argument count.
u32 encode(Op op, std::initializer_list<s64> fields);
u32 encode(Op op, std::span<const s64> fields);

// Common helpers.
inline bool is_load(Op op) { return (op_info(op).flags & F_LOAD) != 0; }
inline bool is_store(Op op) { return (op_info(op).flags & F_STORE) != 0; }
inline bool is_update(Op op) { return (op_info(op).flags & F_UPD) != 0; }

// Bitmask of GPRs (bit n = rn) that an instruction may write.
u32 gpr_writes(u32 raw, Op op);

}  // namespace gekko
