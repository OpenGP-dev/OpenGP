// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/interpreter.h"

#include <tuple>

#include "gekko/isa.h"
#include "ppc/dispatch.h"
#include "ppc/ops.h"

namespace ppc {

InterpreterOptions g_interpreter_options;

namespace {

using gekko::Op;
namespace f = gekko::fields;

// Branch handlers return the next PC.

u32 branch_b(Context& c, u8* base, u32 pc, u32 raw) {
  const u32 target = (f::aa::get(raw) ? 0 : pc) + static_cast<u32>(f::li::get(raw));
  const bool link = f::lk::get(raw);
  if (link) c.lr = pc + 4;
  if (g_interpreter_options.call_native) {
    if (GuestFunc fn = g_functions.find(target)) {
      fn(c, base);
      return link ? pc + 4 : c.lr;  // call, or tail call (then return)
    }
  }
  check_interrupts(c, base);
  return target;
}

u32 branch_bc(Context& c, u8* base, u32 pc, u32 raw) {
  const bool link = f::lk::get(raw);
  const bool taken = bc_cond(c, f::bo::get(raw), f::bi::get(raw));
  if (link) c.lr = pc + 4;
  if (!taken) return pc + 4;
  const u32 target = (f::aa::get(raw) ? 0 : pc) + static_cast<u32>(f::bd::get(raw));
  if (g_interpreter_options.call_native) {
    if (GuestFunc fn = g_functions.find(target)) {
      fn(c, base);
      return link ? pc + 4 : c.lr;
    }
  }
  check_interrupts(c, base);
  return target;
}

u32 branch_bclr(Context& c, u8* base, u32 pc, u32 raw) {
  const bool link = f::lk::get(raw);
  const bool taken = bc_cond(c, f::bo::get(raw), f::bi::get(raw));
  const u32 target = c.lr & ~3u;
  if (link) c.lr = pc + 4;
  if (!taken) return pc + 4;
  if (link && g_interpreter_options.call_native) {
    if (GuestFunc fn = g_functions.find(target)) {
      fn(c, base);
      return pc + 4;
    }
  }
  return target;
}

u32 branch_bcctr(Context& c, u8* base, u32 pc, u32 raw) {
  const bool link = f::lk::get(raw);
  const bool taken = bcctr_cond(c, f::bo::get(raw), f::bi::get(raw));
  const u32 target = c.ctr & ~3u;
  if (link) c.lr = pc + 4;
  if (!taken) return pc + 4;
  if (g_interpreter_options.call_native) {
    if (GuestFunc fn = g_functions.find(target)) {
      fn(c, base);
      return link ? pc + 4 : c.lr;
    }
  }
  check_interrupts(c, base);
  return target;
}

u32 branch_sc(Context& c, u8* base, u32 pc, u32) {
  g_hooks.syscall(c, base, pc);
  return pc + 4;
}

u32 branch_rfi(Context& c, u8* base, u32 pc, u32) {
  g_hooks.rfi(c, base, pc);
  c.msr = c.spr[spr::SRR1];
  return c.spr[spr::SRR0] & ~3u;
}

}  // namespace

void interpret_until(Context& c, u8* base, u32 pc, u32 return_address, u32 return_sp) {
  for (;;) {
    if (pc == return_address && c.gpr[1] == return_sp) return;
    if ((pc >> 28) != 0x8 || (pc & 3)) fatal("interpreter: bad PC %08X", pc);
    c.pc = pc;
    const u32 raw = load_be<u32>(base + pc);
    u32 next = pc + 4;

    switch (gekko::decode(raw)) {
#define INVOKE_CTX(id, sig) std::apply([&](auto... a) { op::id(c, a...); }, gekko::sigs::sig::args(raw))
#define INVOKE_MEM(id, sig) \
  std::apply([&](auto... a) { op::id<Mem::Checked>(c, base, a...); }, gekko::sigs::sig::args(raw))
#define INVOKE_BASE(id, sig) std::apply([&](auto... a) { op::id(c, base, a...); }, gekko::sigs::sig::args(raw))
#define INVOKE_PC(id, sig) std::apply([&](auto... a) { op::id(c, base, pc, a...); }, gekko::sigs::sig::args(raw))
#define INVOKE_BRANCH(id, sig) next = branch_##id(c, base, pc, raw)
#define GEKKO_OP(id, mn, opcd, xo, kind, sig, cc, flags) \
  case Op::id:                                           \
    INVOKE_##cc(id, sig);                                \
    break;
#include "gekko/ops.inc"
#undef GEKKO_OP
#undef INVOKE_CTX
#undef INVOKE_MEM
#undef INVOKE_BASE
#undef INVOKE_PC
#undef INVOKE_BRANCH
      case Op::Invalid:
      case Op::Count:
        fatal("interpreter: invalid instruction %08X at %08X", raw, pc);
    }
    pc = next;
  }
}

void interpret(Context& c, u8* base, u32 entry) { interpret_until(c, base, entry, c.lr, c.gpr[1]); }

}  // namespace ppc
