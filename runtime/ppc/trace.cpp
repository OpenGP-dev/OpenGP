// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/trace.h"

#include <algorithm>
#include <memory>
#include <mutex>

namespace ppc {

namespace {
std::mutex g_registry_mutex;
std::vector<std::unique_ptr<TraceStack>> g_registry;
}  // namespace

TraceStack& trace_stack() {
  thread_local TraceStack* stack = [] {
    auto owned = std::make_unique<TraceStack>();
    TraceStack* raw = owned.get();
    std::lock_guard lock(g_registry_mutex);
    g_registry.push_back(std::move(owned));  // kept for the process lifetime
    return raw;
  }();
  return *stack;
}

std::vector<u32> trace_snapshot(const TraceStack& stack) {
  const u32 depth = std::min(stack.depth.load(std::memory_order_relaxed), TraceStack::kCapacity);
  return std::vector<u32>(stack.frames, stack.frames + depth);
}

std::vector<TraceStack*> trace_all_stacks() {
  std::lock_guard lock(g_registry_mutex);
  std::vector<TraceStack*> out;
  for (const auto& s : g_registry) out.push_back(s.get());
  return out;
}

}  // namespace ppc
