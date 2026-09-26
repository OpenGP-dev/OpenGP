// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenGP launcher for Mario Kart Arcade GP 2.
//
// Milestone 2: loads main.dol into guest memory and installs the recompiled
// code. With --run it also jumps to the entry point, printing hardware
// accesses and OSReport output, and stops after a timeout. The hardware
// itself is not emulated yet (milestone 3), so the game will stop or stall
// early; the output shows how far it gets.
//
//   opengp-mkgp2 [--dol PATH] [--run] [--timeout SECONDS] [--mmio-log N]

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "ppc/memory.h"
#include "ppc/module.h"
#include "ppc_recomp_shared.h"

namespace {

using namespace ppc;

// Map the largest MEM1 Dolphin supports; how much the game is told it has is
// decided when boot is emulated (milestone 3).
constexpr u32 kMem1Size = 0x04000000;

u8* g_base = nullptr;
std::atomic<u64> g_mmio_count{0};
std::atomic<u32> g_last_mmio{0};
std::atomic<u64> g_gather_bytes{0};
u64 g_mmio_log_limit = 64;

void log_mmio(const char* kind, u32 addr, u32 value, u32 size) {
  const u64 n = g_mmio_count.fetch_add(1);
  g_last_mmio = addr;
  if (n < g_mmio_log_limit)
    std::fprintf(stderr, "[mmio] %s%u %08X = %08X\n", kind, size * 8, addr, value);
  else if (n == g_mmio_log_limit)
    std::fprintf(stderr, "[mmio] (further accesses not shown)\n");
}

u32 mmio_read(u32 addr, u32 size) {
  log_mmio("read", addr, 0, size);
  return 0;
}
void mmio_write(u32 addr, u32 value, u32 size) { log_mmio("write", addr, value, size); }
void gather_pipe_write(const u8*, u32 size) { g_gather_bytes += size; }

void print_status(const char* why) {
  std::fprintf(stderr, "[opengp] %s: %llu MMIO accesses (last %08X), %llu bytes to the GPU FIFO\n", why,
               static_cast<unsigned long long>(g_mmio_count.load()), g_last_mmio.load(),
               static_cast<unsigned long long>(g_gather_bytes.load()));
}

void fatal_hook(const char* message) {
  std::fprintf(stderr, "[opengp] fatal: %s\n", message);
  print_status("state at stop");
  std::fflush(stderr);
}

std::string read_c_string(u32 addr, std::size_t max = 4096) {
  std::string s;
  while (s.size() < max) {
    if (!(addr >= MEM1_CACHED && addr < MEM1_CACHED + kMem1Size) &&
        !(addr >= MEM1_UNCACHED && addr < MEM1_UNCACHED + kMem1Size))
      break;
    const char ch = static_cast<char>(g_base[addr]);
    if (!ch) break;
    s += ch;
    ++addr;
  }
  return s;
}

// Minimal printf for guest variadic calls (PowerPC EABI): integer arguments
// come from r4..r10 then the caller's stack, floating-point ones from f1..f8.
std::string format_guest(Context& ctx, u32 fmt_addr) {
  const std::string fmt = read_c_string(fmt_addr);
  u32 next_gpr = 4, next_fpr = 1;
  u32 stack = ctx.gpr[1] + 8;
  auto next_int = [&]() -> u32 {
    if (next_gpr <= 10) return ctx.gpr[next_gpr++];
    const u32 v = load_be<u32>(g_base + stack);
    stack += 4;
    return v;
  };
  auto next_double = [&]() -> double {
    if (next_fpr <= 8) return ctx.fpr[next_fpr++].d0();
    stack = (stack + 7) & ~7u;
    const u64 v = load_be<u64>(g_base + stack);
    stack += 8;
    return std::bit_cast<double>(v);
  };
  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    std::string spec = "%";
    ++i;
    while (i < fmt.size() && std::strchr("-+ #0123456789.", fmt[i])) spec += fmt[i++];
    while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i])) ++i;  // host sizes differ; drop modifiers
    if (i >= fmt.size()) break;
    const char conv = fmt[i];
    char buf[512];
    switch (conv) {
      case 'd':
      case 'i': std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int>(next_int())); break;
      case 'u':
      case 'x':
      case 'X':
      case 'o': std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_int()); break;
      case 'c': std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), static_cast<int>(next_int())); break;
      case 'p': std::snprintf(buf, sizeof(buf), "0x%08X", next_int()); break;
      case 's': std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), read_c_string(next_int()).c_str()); break;
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G': std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_double()); break;
      case '%': std::snprintf(buf, sizeof(buf), "%%"); break;
      default: std::snprintf(buf, sizeof(buf), "%s%c", spec.c_str(), conv); break;
    }
    out += buf;
  }
  return out;
}

bool read_file(const std::string& path, std::vector<u8>* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

}  // namespace

// Replaces the recompiled OSReport (weak symbol) with a host implementation.
namespace guest {
PPC_FUNC(OSReport) {
  const std::string text = format_guest(ctx, ctx.gpr[3]);
  std::fputs(("[OSReport] " + text).c_str(), stdout);
  if (text.empty() || text.back() != '\n') std::fputc('\n', stdout);
  std::fflush(stdout);
}
}  // namespace guest

int main(int argc, char** argv) {
  std::string dol_path = OPENGP_DEFAULT_DOL;
  bool run = false;
  int timeout_s = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dol" && i + 1 < argc) dol_path = argv[++i];
    else if (a == "--run") run = true;
    else if (a == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
    else if (a == "--mmio-log" && i + 1 < argc) g_mmio_log_limit = std::strtoull(argv[++i], nullptr, 10);
    else {
      std::fprintf(stderr, "usage: opengp-mkgp2 [--dol PATH] [--run] [--timeout SECONDS] [--mmio-log N]\n");
      return 2;
    }
  }

  std::vector<u8> dol;
  if (!read_file(dol_path, &dol)) {
    std::fprintf(stderr, "cannot read %s\n", dol_path.c_str());
    return 1;
  }
  GuestMemory mem;
  std::string error;
  if (!mem.init(kMem1Size, &error)) {
    std::fprintf(stderr, "guest memory: %s\n", error.c_str());
    return 1;
  }
  g_base = mem.base();
  u32 entry = 0;
  if (!load_dol(dol.data(), dol.size(), g_base, &entry, &error)) {
    std::fprintf(stderr, "%s: %s\n", dol_path.c_str(), error.c_str());
    return 1;
  }
  install_module(guest::g_module, g_base);

  const bool osreport_replaced = g_functions.find(0x8002CE3C) == &guest::OSReport &&
                                 &guest::OSReport != &guest::__imp__OSReport;
  std::printf("OpenGP: Mario Kart Arcade GP 2 (GGPE02)\n");
  std::printf("  main.dol:          %s (%zu bytes)\n", dol_path.c_str(), dol.size());
  std::printf("  entry point:       %08X\n", entry);
  std::printf("  recompiled:        %zu functions, %zu patches\n", guest::g_module.function_count,
              guest::g_module.patch_count);
  std::printf("  OSReport replaced: %s\n", osreport_replaced ? "yes" : "NO (weak symbol override failed)");
  if (!run) {
    std::printf("Pass --run to jump to the entry point (hardware is not emulated yet).\n");
    return osreport_replaced ? 0 : 1;
  }

  reset_hooks();
  g_hooks.mmio_read = mmio_read;
  g_hooks.mmio_write = mmio_write;
  g_hooks.gather_pipe_write = gather_pipe_write;
  g_hooks.fatal = fatal_hook;
  // The Dolphin SDK only uses `sc` to synchronise the caches (DCFlushRange,
  // PPCSync): its handler toggles HID0 and returns. Caches are not modelled.
  g_hooks.syscall = [](Context&, u8*, u32) {};

  std::thread([timeout_s] {
    std::this_thread::sleep_for(std::chrono::seconds(timeout_s));
    print_status("timeout");
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
  }).detach();

  Context ctx;
  reset_context(ctx);
  std::printf("Jumping to %08X...\n", entry);
  std::fflush(stdout);
  call(ctx, g_base, entry);
  print_status("entry point returned");
  return 0;
}
