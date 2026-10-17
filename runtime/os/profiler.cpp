// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/profiler.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "os/stack_walk.h"

namespace os {

namespace {

constexpr int kMaxFrames = 24;

struct ThreadSamples {
  HANDLE handle = nullptr;
  std::unordered_map<DWORD64, unsigned> by_address;
  std::vector<std::array<DWORD64, kMaxFrames>> stacks;  // return addresses (0-terminated)
  unsigned total = 0;
};

// Walks a suspended thread's stack from its context.
void unwind(const CONTEXT& context, std::array<DWORD64, kMaxFrames>& frames) {
  frames.fill(0);
  walk_stack(context, frames.data(), kMaxFrames);
}

std::string symbol_name(HANDLE process, DWORD64 address, DWORD64* start) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
  auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
  info->SizeOfStruct = sizeof(SYMBOL_INFO);
  info->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, info)) {
    *start = address - displacement;
    return std::string(info->Name, info->NameLen);
  }
  // Not in a module with symbols: group by 4 KiB page.
  *start = address & ~DWORD64{0xFFF};
  char name[32];
  std::snprintf(name, sizeof(name), "?%llX", static_cast<unsigned long long>(*start));
  return name;
}

void run(double start_s, double duration_s) {
  std::this_thread::sleep_for(std::chrono::duration<double>(start_s));
  std::fprintf(stderr, "[profile] sampling for %.0f s\n", duration_s);
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  std::map<DWORD, ThreadSamples> threads;
  timeBeginPeriod(1);

  const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration_s);
  auto next_scan = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < end) {
    if (std::chrono::steady_clock::now() >= next_scan) {
      // Pick up new threads.
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self || threads.count(entry.th32ThreadID)) continue;
        const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                    entry.th32ThreadID);
        if (h) threads[entry.th32ThreadID].handle = h;
      }
      CloseHandle(snapshot);
      next_scan += std::chrono::seconds(1);
    }
    for (auto& [id, t] : threads) {
      if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t.handle, &context)) {
        ++t.by_address[context.Rip];
        ++t.total;
        unwind(context, t.stacks.emplace_back());
      }
      ResumeThread(t.handle);
    }
    Sleep(1);
  }
  timeEndPeriod(1);

  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  for (auto& [id, t] : threads) {
    // Group by function; leave out threads that mostly wait.
    std::unordered_map<std::string, unsigned> by_function;
    unsigned waiting = 0;
    for (const auto& [address, count] : t.by_address) {
      DWORD64 start = 0;
      const std::string name = symbol_name(process, address, &start);
      if (name.rfind("Nt", 0) == 0 || name.rfind("Zw", 0) == 0) waiting += count;  // system calls: waits
      by_function[name] += count;
    }
    const unsigned busy = t.total - waiting;
    if (t.total == 0 || busy * 50 < t.total) continue;
    std::vector<std::pair<std::string, unsigned>> sorted(by_function.begin(), by_function.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[profile] thread %lu: %u samples, %.0f%% busy\n", id, t.total, 100.0 * busy / t.total);
    for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 40); ++i) {
      std::fprintf(stderr, "[profile]   %5.1f%%  %s\n", 100.0 * sorted[i].second / t.total, sorted[i].first.c_str());
    }
    // Inclusive time: every function on the stack gets the sample (once).
    std::unordered_map<DWORD64, std::string> names;
    std::unordered_map<std::string, unsigned> inclusive;
    for (const auto& stack : t.stacks) {
      std::vector<const std::string*> seen;
      for (const DWORD64 address : stack) {
        if (!address) break;
        auto it = names.find(address);
        if (it == names.end()) {
          DWORD64 start = 0;
          it = names.emplace(address, symbol_name(process, address, &start)).first;
        }
        const std::string& name = it->second;
        if (std::find_if(seen.begin(), seen.end(), [&](const std::string* n) { return *n == name; }) != seen.end()) {
          continue;
        }
        seen.push_back(&name);
        ++inclusive[name];
      }
    }
    sorted.assign(inclusive.begin(), inclusive.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(stderr, "[profile] thread %lu, inclusive:\n", id);
    for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 60); ++i) {
      std::fprintf(stderr, "[profile]   %5.1f%%  %s\n", 100.0 * sorted[i].second / t.total, sorted[i].first.c_str());
    }
  }
  SymCleanup(process);
  for (auto& [id, t] : threads) CloseHandle(t.handle);
  std::fflush(stderr);
}

}  // namespace

void start_profiler_from_env() {
  const char* spec = std::getenv("OPENGP_PROFILE");
  if (!spec) return;
  double start_s = 0, duration_s = 10;
  if (std::sscanf(spec, "%lf,%lf", &start_s, &duration_s) < 1) return;
  std::thread(run, start_s, duration_s).detach();
}

}  // namespace os

#else

namespace os {
void start_profiler_from_env() {
  if (std::getenv("OPENGP_PROFILE")) std::fprintf(stderr, "[profile] not supported on this platform\n");
}
}  // namespace os

#endif
