// SPDX-License-Identifier: GPL-3.0-or-later
//
// Optional guest call-stack tracing for debugging. When recompiled code is
// built with PPC_TRACE_CALLS defined, every guest function pushes its address
// on a per-host-thread stack on entry and pops it on exit, so a stall or crash
// can be reported as a guest backtrace. Without the define it costs nothing.
#pragma once

#include <atomic>
#include <vector>

#include "ppc/context.h"

namespace ppc {

struct TraceStack {
  static constexpr u32 kCapacity = 1024;
  u32 frames[kCapacity];
  std::atomic<u32> depth{0};  // may exceed kCapacity; frames beyond it are not recorded
};

// The calling host thread's stack (registered on first use).
TraceStack& trace_stack();

// Copies a stack's frames, innermost last. Safe to call from another thread
// (the result may be torn if the owner is running; it is a diagnostic).
std::vector<u32> trace_snapshot(const TraceStack& stack);

// All stacks registered so far (one per host thread that ran guest code).
std::vector<TraceStack*> trace_all_stacks();

struct TraceScope {
  explicit TraceScope(u32 addr) : stack_(trace_stack()) {
    const u32 d = stack_.depth.load(std::memory_order_relaxed);
    if (d < TraceStack::kCapacity) stack_.frames[d] = addr;
    stack_.depth.store(d + 1, std::memory_order_relaxed);
  }
  ~TraceScope() { stack_.depth.store(stack_.depth.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed); }
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  TraceStack& stack_;
};

}  // namespace ppc

#ifdef PPC_TRACE_CALLS
#define PPC_TRACE_ENTER(addr) ::ppc::TraceScope ppc_trace_scope_(addr)
#else
#define PPC_TRACE_ENTER(addr) ((void)0)
#endif
