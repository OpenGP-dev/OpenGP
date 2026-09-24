// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/hooks.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace ppc {

namespace {

// 40.5 MHz: the Gekko time base ticks at bus clock / 4 (162 MHz / 4).
constexpr u64 kTimebaseHz = 40500000;

u64 g_timebase_offset = 0;

u64 host_ticks() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return static_cast<u64>(ns) * kTimebaseHz / 1000000000ULL;
}

u32 default_mmio_read(u32 addr, u32 size) {
  std::fprintf(stderr, "[ppc] unhandled MMIO read%u @ %08X\n", size * 8, addr);
  return 0;
}

void default_mmio_write(u32 addr, u32 value, u32 size) {
  std::fprintf(stderr, "[ppc] unhandled MMIO write%u @ %08X = %08X\n", size * 8, addr, value);
}

void default_gather_pipe_write(const u8* bytes, u32 size) {
  g_default_gather_pipe.insert(g_default_gather_pipe.end(), bytes, bytes + size);
}

u64 default_read_timebase() { return host_ticks() + g_timebase_offset; }
void default_write_timebase(u64 value) { g_timebase_offset = value - host_ticks(); }

u32 g_decrementer = 0;
u32 default_read_decrementer() { return g_decrementer; }
void default_write_decrementer(u32 value) { g_decrementer = value; }

void default_trap(Context&, u8*, u32 pc) { fatal("trap instruction at %08X", pc); }
void default_syscall(Context&, u8*, u32 pc) { fatal("sc at %08X (no system call handler)", pc); }
void default_rfi(Context&, u8*, u32 pc) { fatal("rfi at %08X (exception handlers are not emulated)", pc); }
void default_service_interrupts(Context&, u8*) { g_interrupt_pending.store(false); }
void default_fatal(const char* message) { std::fprintf(stderr, "[ppc] fatal: %s\n", message); }

}  // namespace

Hooks g_hooks;
std::atomic<bool> g_interrupt_pending{false};
std::vector<u8> g_default_gather_pipe;

void reset_hooks() {
  g_hooks = Hooks{
      .mmio_read = default_mmio_read,
      .mmio_write = default_mmio_write,
      .gather_pipe_write = default_gather_pipe_write,
      .read_timebase = default_read_timebase,
      .write_timebase = default_write_timebase,
      .read_decrementer = default_read_decrementer,
      .write_decrementer = default_write_decrementer,
      .trap = default_trap,
      .syscall = default_syscall,
      .rfi = default_rfi,
      .msr_changed = nullptr,
      .icache_invalidate = nullptr,
      .service_interrupts = default_service_interrupts,
      .fatal = default_fatal,
  };
  g_interrupt_pending.store(false);
}

namespace {
// Install the defaults before main().
struct DefaultHooksInstaller {
  DefaultHooksInstaller() { reset_hooks(); }
} g_install_default_hooks;
}  // namespace

void fatal(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (g_hooks.fatal) g_hooks.fatal(buf);
  std::abort();
}

}  // namespace ppc
