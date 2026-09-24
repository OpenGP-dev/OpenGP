// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/dispatch.h"

#include <algorithm>

#include "ppc/interpreter.h"

namespace ppc {

FunctionTable g_functions;

void FunctionTable::grow(u32 lo, u32 hi) {
  lo &= ~3u;
  hi = (hi + 3) & ~3u;
  if (span_ != 0) {
    lo = std::min(lo, lo_);
    hi = std::max(hi, lo_ + span_);
  }
  if (span_ != 0 && lo == lo_ && hi == lo_ + span_) return;
  std::vector<GuestFunc> table((hi - lo) / 4, nullptr);
  for (u32 i = 0; i < table_.size(); ++i) table[(lo_ - lo) / 4 + i] = table_[i];
  table_ = std::move(table);
  lo_ = lo;
  span_ = hi - lo;
}

void FunctionTable::add(std::span<const FuncMapping> mappings) {
  if (mappings.empty()) return;
  u32 lo = 0xFFFFFFFF, hi = 0;
  for (const FuncMapping& m : mappings) {
    lo = std::min(lo, m.address);
    hi = std::max(hi, m.address + 4);
  }
  grow(lo, hi);
  for (const FuncMapping& m : mappings) table_[(m.address - lo_) / 4] = m.func;
}

void FunctionTable::add(u32 address, GuestFunc func) {
  grow(address, address + 4);
  table_[(address - lo_) / 4] = func;
}

void FunctionTable::clear() {
  table_.clear();
  lo_ = 0;
  span_ = 0;
}

void call(Context& ctx, u8* base, u32 target) {
  if (GuestFunc f = g_functions.find(target)) {
    f(ctx, base);
    return;
  }
  interpret(ctx, base, target);
}

void jump(Context& ctx, u8* base, u32 target, u32 return_address, u32 return_sp) {
  interpret_until(ctx, base, target, return_address, return_sp);
}

}  // namespace ppc
