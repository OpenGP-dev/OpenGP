// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/hang_watch.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "hw/di.h"
#include "hw/vi.h"
#include "machine.h"
#include "os/activity.h"

namespace app {

using namespace gekko;

namespace {

// The SDK's globals in low memory.
constexpr u32 kCurrentThread = 0x800000E4;  // OSGetCurrentThread()
constexpr u32 kActiveThreads = 0x800000DC;  // __OSActiveThreadQueue.head
// OSThread (Dolphin SDK <dolphin/os/OSThread.h>; its OSContext comes first).
constexpr u32 kThreadSp = 0x004;            // context.gpr[1]
constexpr u32 kThreadLr = 0x084;            // context.lr
constexpr u32 kThreadSrr0 = 0x198;          // context.srr0: where it resumes
constexpr u32 kThreadState = 0x2C8;         // u16
constexpr u32 kThreadPriority = 0x2D0;
constexpr u32 kThreadQueue = 0x2DC;         // the queue it waits on
constexpr u32 kThreadNextActive = 0x2FC;    // linkActive.next

constexpr u32 MSR_EE = 0x8000;

std::string format(const char* fmt, auto... args) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return buf;
}

std::string function_at(const FunctionNames& names, u32 addr) {
  auto it = names.upper_bound(addr);
  if (it == names.begin()) return format("%08X", addr);
  --it;
  return format("%08X %s+0x%X", addr, it->second.c_str(), addr - it->first);
}

const char* state_name(u16 state) {
  switch (state) {
    case 1: return "ready";
    case 2: return "running";
    case 4: return "waiting";
    case 8: return "exiting";
    default: return "?";
  }
}

}  // namespace

std::string describe_threads(opengp::Machine& machine, const FunctionNames& names) {
  const u8* base = machine.base();
  const u32 end = 0x80000000u + machine.mem1_size();
  auto valid = [&](u32 a) { return a >= 0x80000000u && a + 8 <= end && (a & 3) == 0; };
  auto word = [&](u32 a) { return valid(a) ? ppc::load_be<u32>(base + a) : 0u; };
  // Return addresses from the back chain: each frame starts with the caller's
  // frame, whose second word is where the frame's function returns to.
  auto backtrace = [&](std::string& out, u32 sp) {
    for (int depth = 0; depth < 32 && valid(sp); ++depth) {
      const u32 caller = word(sp);
      if (!valid(caller) || caller <= sp) break;
      out += "      " + function_at(names, word(caller + 4)) + "\n";
      sp = caller;
    }
  };

  std::string out;
  hw::Hardware& hw = machine.hardware();
  const hw::ViDevice* vi = hw.device<hw::ViDevice>();
  hw::DiDevice* di = hw.device<hw::DiDevice>();
  out += format("retraces %llu, frames %llu, exceptions %llu, idle waits %llu, PI cause %05X mask %05X%s\n",
                static_cast<unsigned long long>(vi ? vi->retrace_count() : 0),
                static_cast<unsigned long long>(machine.gpu().frames()),
                static_cast<unsigned long long>(machine.exceptions_delivered()),
                static_cast<unsigned long long>(machine.threads().idle_waits()), hw.interrupt_cause(),
                hw.interrupt_mask(), hw.paused() ? ", paused" : "");
  if (di) out += format("DI status %08X control %08X\n", di->read(0x00, 4), di->read(0x1C, 4));

  int id = -1;
  if (const ppc::Context* c = machine.threads().running_context(&id)) {
    // Read while it may be running: a snapshot.
    const u32 sp = c->gpr[1], lr = c->lr, msr = c->msr;
    out += format("CPU: host thread %d, interrupts %s, sp %08X, lr %s\n", id, (msr & MSR_EE) ? "on" : "off", sp,
                  function_at(names, lr).c_str());
    backtrace(out, sp);
  }

  // The host side: what the main thread (window, renderer) is doing, and
  // whether the game's thread is waiting on it.
  out += os::g_main_activity.describe("main thread");
  out += os::g_guest_wait.describe("game thread");

  const u32 current = word(kCurrentThread);
  u32 t = word(kActiveThreads);
  for (int n = 0; n < 32 && valid(t); ++n, t = word(t + kThreadNextActive)) {
    const u16 state = valid(t) ? ppc::load_be<u16>(base + t + kThreadState) : 0;
    out += format("thread %08X%s: %s, priority %d, waiting on %08X, resumes at %s\n", t,
                  t == current ? " (current)" : "", state_name(state), static_cast<int>(word(t + kThreadPriority)),
                  word(t + kThreadQueue), function_at(names, word(t + kThreadSrr0)).c_str());
    // The current thread's saved registers are stale: it runs on the CPU (above).
    if (t == current) continue;
    out += "      lr " + function_at(names, word(t + kThreadLr)) + "\n";
    backtrace(out, word(t + kThreadSp));
  }
  return out;
}

void start_hang_watch(opengp::Machine& machine, const FunctionNames& names, std::filesystem::path path,
                      int seconds) {
  std::thread([&machine, &names, path = std::move(path), seconds] {
    using clock = std::chrono::steady_clock;
    u64 frames = 0;
    auto since = clock::now();
    bool reported = false;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const u64 now_frames = machine.gpu().frames();
      if (now_frames != frames || machine.hardware().paused()) {
        frames = now_frames;
        since = clock::now();
        reported = false;
        continue;
      }
      // Not before the first frame (start-up), and once per hang.
      if (frames == 0 || reported || clock::now() - since < std::chrono::seconds(seconds)) continue;
      reported = true;
      // Two looks a second apart: which counters still move.
      std::string report = format("OpenGP: no frame for %d seconds (frame %llu)\n\n", seconds,
                                  static_cast<unsigned long long>(frames));
      report += describe_threads(machine, names);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      report += "\nA second later:\n" + describe_threads(machine, names);
      std::fprintf(stderr, "\n[opengp] %s\n", report.c_str());
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      file << report;
      if (file) std::fprintf(stderr, "[opengp] hang report written to %s\n", path.string().c_str());
      std::fflush(stderr);
    }
  }).detach();
}

}  // namespace app
