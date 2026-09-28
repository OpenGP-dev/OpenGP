// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/threads.h"

#include <cstdio>
#include <thread>

#include "ppc/dispatch.h"
#include "ppc/hooks.h"
#include "ppc/memory.h"
#include "ppc/ops.h"

namespace os {

namespace {

// OSContext layout (Dolphin SDK <dolphin/os/OSContext.h>).
constexpr u32 CTX_GPR = 0x000;
constexpr u32 CTX_CR = 0x080;
constexpr u32 CTX_LR = 0x084;
constexpr u32 CTX_CTR = 0x088;
constexpr u32 CTX_XER = 0x08C;
constexpr u32 CTX_FPR = 0x090;
constexpr u32 CTX_FPSCR = 0x194;
constexpr u32 CTX_SRR0 = 0x198;
constexpr u32 CTX_SRR1 = 0x19C;
constexpr u32 CTX_STATE = 0x1A2;  // u16
constexpr u32 CTX_GQR = 0x1A4;
constexpr u32 CTX_PSF = 0x1C8;
constexpr u16 CTX_STATE_FPSAVED = 0x1;
constexpr u16 CTX_STATE_EXC = 0x2;

// OSThread: the context is its first member; state is a u16 at 0x2C8.
constexpr u32 THREAD_STATE = 0x2C8;
constexpr u16 THREAD_STATE_MORIBUND = 8;

// Low-memory OS globals.
constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_EXCEPTION_TABLE = 0x80003000;

constexpr u32 MSR_EE = 0x8000;

thread_local HostThread* t_self = nullptr;

// Processor-wide state that is not per-thread: SPRs other than the GQRs
// (which the SDK saves per context) and the exception save registers.
void copy_global_state(const ppc::Context& from, ppc::Context& to) {
  for (u32 i = 0; i < 1024; ++i) {
    if ((i >= ppc::spr::GQR0 && i < ppc::spr::GQR0 + 8) || i == ppc::spr::SRR0 || i == ppc::spr::SRR1) continue;
    to.spr[i] = from.spr[i];
  }
  for (u32 i = 0; i < 16; ++i) to.sr[i] = from.sr[i];
}

}  // namespace

struct HostThread {
  int id = 0;
  ppc::Context* ctx = nullptr;             // registers while this thread runs guest code
  std::unique_ptr<ppc::Context> storage;  // owned context for spawned threads
  u32 saved_ctx = 0;                       // context last passed to OSSaveContext
  bool in_interrupt = false;
  u32 interrupt_ctx = 0;
  bool exit_requested = false;
};

ThreadManager* g_threads = nullptr;

ThreadManager::ThreadManager(hw::Hardware& hw, u8* base) : hw_(hw), base_(base) { g_threads = this; }

ThreadManager::~ThreadManager() {
  if (g_threads == this) g_threads = nullptr;
}

u32 ThreadManager::read32(u32 addr) const { return ppc::load_be<u32>(base_ + addr); }
void ThreadManager::write32(u32 addr, u32 value) { ppc::store_be<u32>(base_ + addr, value); }

void ThreadManager::run(ppc::Context& ctx, u32 entry) {
  auto main = std::make_unique<HostThread>();
  main->id = 0;
  main->ctx = &ctx;
  t_self = main.get();
  running_ = main.get();
  threads_.push_back(std::move(main));

  ppc::g_hooks.service_interrupts = [](ppc::Context& c, u8*) { g_threads->service_interrupts(c); };
  ppc::g_hooks.msr_changed = [](ppc::Context& c, u8*, u32 old_msr) {
    if (!(old_msr & MSR_EE) && (c.msr & MSR_EE) && ppc::g_interrupt_pending.load(std::memory_order_acquire))
      g_threads->service_interrupts(c);
  };

  try {
    ppc::call(ctx, base_, entry);
  } catch (ThreadExit&) {
    // The first guest thread exited; the others keep running on their own
    // host threads. Keep this one parked.
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [] { return false; });
  }
}

void ThreadManager::on_save_context(ppc::Context&, u32 context) {
  if (t_self) t_self->saved_ctx = context;
}

void ThreadManager::idle(ppc::Context&) { hw_.wait_for_interrupt(4); }

void ThreadManager::load_context(ppc::Context& ctx, u32 target) {
  HostThread* self = t_self;
  // Like the real OSLoadContext, clear the "saved by an exception" flag.
  const u16 state = ppc::load_be<u16>(base_ + target + CTX_STATE);
  ppc::store_be<u16>(base_ + target + CTX_STATE, static_cast<u16>(state & ~CTX_STATE_EXC));

  // Returning from an exception to the interrupted context, or resuming
  // ourselves: just return to the caller.
  if (self->in_interrupt && target == self->interrupt_ctx) {
    // The recompiled caller returns straight back to deliver() (see
    // Context::exception_return); the interpreter would carry on.
    ctx.exception_return = true;
    return;
  }
  const u32 me = self->in_interrupt ? self->interrupt_ctx : self->saved_ctx;
  if (target == me) return;

  // Find the host thread that owns `target`.
  HostThread* next = nullptr;
  if (auto it = owners_.find(target); it != owners_.end()) {
    const Owner owner = it->second;
    owners_.erase(it);
    const bool unchanged = read32(target + CTX_SRR0) == owner.srr0 && read32(target + CTX_GPR + 4) == owner.sp;
    if (owner.thread != self && unchanged) {
      next = owner.thread;
    } else if (owner.thread != self) {
      std::lock_guard lock(mutex_);  // the context was re-created: retire its old host thread
      owner.thread->exit_requested = true;
      cv_.notify_all();
    }
  }

  // Remember how to resume this thread, unless its guest thread is exiting.
  const bool exiting = me != 0 && ppc::load_be<u16>(base_ + me + THREAD_STATE) == THREAD_STATE_MORIBUND;
  if (me == 0) std::fprintf(stderr, "[os] warning: switching away from a thread with no saved context\n");
  if (me != 0 && !exiting) owners_[me] = Owner{self, read32(me + CTX_SRR0), read32(me + CTX_GPR + 4)};

  if (!next) next = spawn(ctx, target);
  hand_off(self, next, exiting);
  if (exiting) throw ThreadExit{};
  // Resumed: someone loaded our context. Inside an exception handler that
  // is the interrupted context, so go back to the interrupted code.
  if (self->in_interrupt) throw ExceptionReturn{};
}

void ThreadManager::hand_off(HostThread* self, HostThread* next, bool self_exits) {
  std::unique_lock lock(mutex_);
  copy_global_state(*self->ctx, *next->ctx);
  running_ = next;
  cv_.notify_all();
  if (self_exits) return;
  cv_.wait(lock, [&] { return running_ == self || self->exit_requested; });
  if (running_ != self) throw ThreadExit{};
}

HostThread* ThreadManager::spawn(ppc::Context& from, u32 context) {
  auto t = std::make_unique<HostThread>();
  t->id = static_cast<int>(threads_.size());
  t->storage = std::make_unique<ppc::Context>();
  t->ctx = t->storage.get();
  ppc::Context& c = *t->ctx;
  ppc::reset_context(c);
  copy_global_state(from, c);

  // Load the registers OSLoadContext would.
  for (u32 i = 0; i < 32; ++i) c.gpr[i] = read32(context + CTX_GPR + 4 * i);
  c.cr = read32(context + CTX_CR);
  c.lr = read32(context + CTX_LR);
  c.ctr = read32(context + CTX_CTR);
  ppc::set_xer(c, read32(context + CTX_XER));
  if (ppc::load_be<u16>(base_ + context + CTX_STATE) & CTX_STATE_FPSAVED) {
    for (u32 i = 0; i < 32; ++i) {
      c.fpr[i].ps0 = ppc::load_be<u64>(base_ + context + CTX_FPR + 8 * i);
      c.fpr[i].ps1 = ppc::load_be<u64>(base_ + context + CTX_PSF + 8 * i);
    }
    c.fpscr = read32(context + CTX_FPSCR);
  }
  for (u32 i = 0; i < 8; ++i) c.spr[ppc::spr::GQR0 + i] = read32(context + CTX_GQR + 4 * i);
  c.msr = read32(context + CTX_SRR1);
  const u32 entry = read32(context + CTX_SRR0);

  HostThread* raw = t.get();
  threads_.push_back(std::move(t));
  std::thread([this, raw, entry] { thread_main(raw, entry); }).detach();
  return raw;
}

void ThreadManager::thread_main(HostThread* t, u32 entry) {
  t_self = t;
  {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return running_ == t || t->exit_requested; });
    if (running_ != t) return;
  }
  try {
    ppc::Context& c = *t->ctx;
    const u32 lr = c.lr;  // where the thread function returns to (OSExitThread)
    ppc::call(c, base_, entry);
    ppc::call(c, base_, lr);
    ppc::fatal("guest thread %d returned past its exit routine", t->id);
  } catch (ThreadExit&) {
  }
}

void ThreadManager::service_interrupts(ppc::Context& ctx) {
  while ((ctx.msr & MSR_EE) && ppc::g_interrupt_pending.load(std::memory_order_acquire)) {
    const hw::Exception e = hw_.take_exception();
    if (e == hw::Exception::None) break;
    deliver(ctx, e);
  }
}

// Emulates the exception vector: save the registers into the current
// OSContext, disable interrupts, and call the SDK's handler for the exception
// with (exception, context). The handler ends in OSLoadContext(context),
// which comes back here (see Context::exception_return and ExceptionReturn);
// then the interrupted registers are restored.
void ThreadManager::deliver(ppc::Context& ctx, hw::Exception e) {
  HostThread* self = t_self;
  const u32 cur = read32(OS_CURRENT_CONTEXT);
  const u32 handler = read32(OS_EXCEPTION_TABLE + 4 * static_cast<u32>(e));
  if (!cur || !handler) return;  // the OS has not installed its handlers yet

  for (u32 i = 0; i < 32; ++i) write32(cur + CTX_GPR + 4 * i, ctx.gpr[i]);
  write32(cur + CTX_CR, ctx.cr);
  write32(cur + CTX_LR, ctx.lr);
  write32(cur + CTX_CTR, ctx.ctr);
  write32(cur + CTX_XER, ppc::get_xer(ctx));
  write32(cur + CTX_SRR0, 0);  // no precise PC in recompiled code
  write32(cur + CTX_SRR1, ctx.msr);
  for (u32 i = 1; i < 8; ++i) write32(cur + CTX_GQR + 4 * i, ctx.spr[ppc::spr::GQR0 + i]);
  const u16 state = ppc::load_be<u16>(base_ + cur + CTX_STATE);
  ppc::store_be<u16>(base_ + cur + CTX_STATE, static_cast<u16>(state | CTX_STATE_EXC));

  const ppc::Context saved = ctx;
  const bool prev_in_interrupt = self->in_interrupt;
  const u32 prev_interrupt_ctx = self->interrupt_ctx;
  self->in_interrupt = true;
  self->interrupt_ctx = cur;

  ctx.spr[ppc::spr::SRR0] = 0;
  ctx.spr[ppc::spr::SRR1] = saved.msr;
  ctx.msr &= ~MSR_EE;
  ctx.gpr[3] = static_cast<u32>(e);
  ctx.gpr[4] = cur;
  ctx.lr = 0;
  ++exceptions_delivered_;
  try {
    ppc::call(ctx, base_, handler);
  } catch (ExceptionReturn&) {
    ++exception_returns_;  // resumed after a thread switch inside the handler
  }

  // Restore the interrupted state, keeping processor-wide SPR changes.
  ppc::Context after = ctx;
  ctx = saved;
  copy_global_state(after, ctx);
  self->in_interrupt = prev_in_interrupt;
  self->interrupt_ctx = prev_interrupt_ctx;
}

}  // namespace os
