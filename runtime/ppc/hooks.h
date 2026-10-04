// SPDX-License-Identifier: GPL-3.0-or-later
//
// Points where the CPU core calls out to the rest of the system (hardware
// registers, the GPU FIFO, timers, exceptions). The defaults are simple
// stand-ins good enough for tests; the hardware layer replaces them.
#pragma once

#include <atomic>
#include <vector>

#include "ppc/context.h"

namespace ppc {

struct Hooks {
  // Hardware registers at 0xCC000000-0xCC007FFF. size is 1, 2 or 4 bytes.
  u32 (*mmio_read)(u32 addr, u32 size);
  void (*mmio_write)(u32 addr, u32 value, u32 size);

  // Write-gather pipe (0xCC008000): `size` big-endian bytes, 1..8.
  void (*gather_pipe_write)(const u8* bytes, u32 size);

  // 64-bit time base (40.5 MHz) and 32-bit decrementer.
  u64 (*read_timebase)();
  void (*write_timebase)(u64 value);
  u32 (*read_decrementer)();
  void (*write_decrementer)(u32 value);

  // Exceptions and supervisor events.
  void (*trap)(Context& ctx, u8* base, u32 pc);     // tw/twi condition met
  void (*syscall)(Context& ctx, u8* base, u32 pc);  // sc
  void (*rfi)(Context& ctx, u8* base, u32 pc);      // rfi executed by guest code
  void (*msr_changed)(Context& ctx, u8* base, u32 old_msr);
  void (*icache_invalidate)(u32 addr);  // icbi

  // Called at safe points when g_interrupt_pending is set.
  void (*service_interrupts)(Context& ctx, u8* base);

  // Unrecoverable guest error. The default prints the message; ppc::fatal()
  // aborts after the hook returns.
  void (*fatal)(const char* message);
};

extern Hooks g_hooks;

// Set by the hardware layer when an interrupt should be delivered; polled by
// recompiled code on loop back-edges and by the interpreter on taken branches.
extern std::atomic<bool> g_interrupt_pending;

// The default gather-pipe hook appends here (used by tests until the GPU exists).
extern std::vector<u8> g_default_gather_pipe;

// Restores the default hooks.
void reset_hooks();

[[noreturn]] void fatal(const char* fmt, ...);

}  // namespace ppc
