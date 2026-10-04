// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mapping from guest function addresses to recompiled host functions, and the
// entry points recompiled code uses for control transfers it cannot resolve
// statically.
#pragma once

#include <span>
#include <vector>

#include "ppc/context.h"
#include "ppc/hooks.h"

namespace ppc {

using GuestFunc = void (*)(Context& ctx, u8* base);

struct FuncMapping {
  u32 address;
  GuestFunc func;
};

// Flat lookup table over the guest code range, one slot per instruction.
class FunctionTable {
 public:
  void add(std::span<const FuncMapping> mappings);
  void add(u32 address, GuestFunc func);  // also used to install HLE overrides
  GuestFunc find(u32 address) const {
    const u32 off = address - lo_;
    if (address < lo_ || off >= span_ || (address & 3)) return nullptr;
    return table_[off >> 2];
  }
  void clear();

 private:
  void grow(u32 lo, u32 hi);
  u32 lo_ = 0;
  u32 span_ = 0;  // bytes covered
  std::vector<GuestFunc> table_;
};

extern FunctionTable g_functions;

// Indirect call (bctrl, blrl, unresolved targets). The caller has already set
// ctx.lr. Runs the recompiled function if there is one, otherwise interprets.
void call(Context& ctx, u8* base, u32 target);

// Transfer of control that does not come back (bctr to an address not known
// at recompile time, or a branch into the middle of another function). A
// recompiled function there is tail-called; anything else is interpreted until
// control returns to `return_address` with r1 == `return_sp`, i.e. until the
// recompiled function that jumped would itself have returned.
void jump(Context& ctx, u8* base, u32 target, u32 return_address, u32 return_sp);

inline void check_interrupts(Context& ctx, u8* base) {
  if (g_interrupt_pending.load(std::memory_order_relaxed)) [[unlikely]]
    g_hooks.service_interrupts(ctx, base);
}

}  // namespace ppc
