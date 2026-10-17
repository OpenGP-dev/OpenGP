// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest threads and exceptions.
//
// The Dolphin SDK schedules threads itself (SelectThread, run queues,
// priorities) and switches between them with OSSaveContext / OSLoadContext.
// Recompiled code keeps guest registers in a host-side ppc::Context and its
// call stack on the host stack, so a context switch cannot simply reload
// registers. Instead every guest thread runs on its own host thread, and
// exactly one host thread executes guest code at a time (it holds "the CPU").
//
//   * OSSaveContext runs as recompiled code; we only note which context the
//     calling host thread saved.
//   * OSLoadContext(ctx) is replaced: it hands the CPU to the host thread that
//     owns `ctx` (starting a new one for a thread that never ran) and blocks
//     until some thread loads this thread's context again, then returns.
//     The SDK's callers ignore what happens after OSLoadContext, so returning
//     to them is safe: they unwind normally.
//   * Interrupts and the decrementer are delivered at safe points (loop
//     back-edges, mtmsr): the registers are saved into the current OSContext
//     as the exception vector would, and the SDK's exception handler is called.
//     Its OSLoadContext of the interrupted context returns straight back to
//     the safe point, which then resumes the interrupted code: it sets
//     Context::exception_return, and recompiled code returns right after a
//     call to OSLoadContext when that is set (`exception_return` in
//     recomp.toml). On hardware OSLoadContext never returns, and the SDK's
//     handlers have code after their early-exit calls to it.
//
// Everything else (scheduling, mutexes, message queues, alarms) stays SDK code.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hw/hardware.h"
#include "ppc/context.h"

namespace os {

using namespace gekko;

struct HostThread;

// Thrown to unwind a host thread whose guest thread has exited or been
// replaced; caught at the host thread's top level.
struct ThreadExit {};

// Thrown when a thread switched away from inside an exception handler is
// resumed: that resumes the interrupted code, so the rest of the handler
// (possibly several calls deep) must not run. Caught by ThreadManager::deliver.
struct ExceptionReturn {};

class ThreadManager {
 public:
  ThreadManager(hw::Hardware& hw, u8* base);
  ~ThreadManager();

  // Runs the game's entry point on the calling thread, which becomes the
  // first guest thread. Returns only if the guest code returns.
  void run(ppc::Context& ctx, u32 entry);

  // Called by the OSSaveContext override before the original runs.
  void on_save_context(ppc::Context& ctx, u32 context);
  // The OSLoadContext replacement.
  void load_context(ppc::Context& ctx, u32 context);
  // Idle-loop hook (top of "while (RunQueueBits == 0)"): waits for an
  // interrupt and takes it, so that the loop sees any thread its handler
  // made ready.
  void idle(ppc::Context& ctx);
  // Delivers pending exceptions if MSR[EE] allows (g_hooks.service_interrupts).
  void service_interrupts(ppc::Context& ctx);

  // Number of guest threads that have been started (for diagnostics).
  int thread_count() const { return static_cast<int>(threads_.size()); }
  // Exceptions delivered, and how many of their handlers had to be unwound
  // with ExceptionReturn (for profiling).
  u64 exceptions_delivered() const { return exceptions_delivered_.load(std::memory_order_relaxed); }
  u64 exception_returns() const { return exception_returns_.load(std::memory_order_relaxed); }
  // How often the idle loop has waited for an interrupt, and for how long in
  // all (ns): the time the game had nothing to do (for diagnostics).
  u64 idle_waits() const { return idle_waits_.load(std::memory_order_relaxed); }
  u64 idle_ns() const { return idle_ns_.load(std::memory_order_relaxed); }
  // The registers of the thread holding the CPU, and its host thread's
  // number (for diagnostics: they change while it runs).
  const ppc::Context* running_context(int* id);

 private:
  struct Owner {
    HostThread* thread;
    u32 srr0;  // context contents when the owner blocked: if they change, the
    u32 sp;    // context was re-created (OSCreateThread) and the owner is stale
  };

  void deliver(ppc::Context& ctx, hw::Exception e);
  HostThread* spawn(ppc::Context& from, u32 context);
  void thread_main(HostThread* t, u32 entry);
  void hand_off(HostThread* self, HostThread* next, bool self_exits);
  u32 read32(u32 addr) const;
  void write32(u32 addr, u32 value);

  hw::Hardware& hw_;
  u8* base_;
  std::mutex mutex_;
  std::condition_variable cv_;
  HostThread* running_ = nullptr;
  std::vector<std::unique_ptr<HostThread>> threads_;
  std::unordered_map<u32, Owner> owners_;  // only touched by the thread holding the CPU
  std::atomic<u64> exceptions_delivered_{0}, exception_returns_{0}, idle_waits_{0}, idle_ns_{0};
  u64 idle_seen_ = 0;  // exceptions_delivered_ when the idle hook last returned
};

extern ThreadManager* g_threads;

}  // namespace os
