// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/crash.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <unistd.h>
#endif

namespace os {

namespace {

const gekko::u8* g_base = nullptr;
CrashCallback g_callback = nullptr;

void describe_guest(std::uintptr_t addr) {
  const auto base = reinterpret_cast<std::uintptr_t>(g_base);
  if (g_base && addr >= base && addr - base < (std::uintptr_t{1} << 32))
    std::fprintf(stderr, "  (guest address %08llX)\n", static_cast<unsigned long long>(addr - base));
}

#ifdef _WIN32

void print_location(const void* p) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(p), &module)) {
    GetModuleFileNameA(module, name, sizeof(name));
    const char* base_name = name;
    for (const char* c = name; *c; ++c)
      if (*c == '\\' || *c == '/') base_name = c + 1;
    std::fprintf(stderr, "%s+0x%llx", base_name,
                 static_cast<unsigned long long>(reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(module)));
  } else {
    std::fprintf(stderr, "%p", p);
  }
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  std::fprintf(stderr, "\n[crash] exception %08lX at ", r->ExceptionCode);
  print_location(r->ExceptionAddress);
  std::fprintf(stderr, " (thread %lu)\n", GetCurrentThreadId());
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    const auto kind = r->ExceptionInformation[0];
    std::fprintf(stderr, "  %s of address %p\n", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                 reinterpret_cast<void*>(r->ExceptionInformation[1]));
    describe_guest(r->ExceptionInformation[1]);
  }
  // The handler runs on the faulting thread's stack, so this backtrace
  // includes the frames that led to the fault.
  void* frames[48];
  const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  std::fprintf(stderr, "  backtrace:\n");
  for (USHORT i = 0; i < n; ++i) {
    std::fprintf(stderr, "    ");
    print_location(frames[i]);
    std::fprintf(stderr, "\n");
  }
  std::fflush(stderr);
  if (g_callback) g_callback();
  std::fflush(stderr);
  std::_Exit(3);
}

#else

void on_signal(int sig, siginfo_t* info, void*) {
  std::fprintf(stderr, "\n[crash] signal %d, address %p\n", sig, info->si_addr);
  describe_guest(reinterpret_cast<std::uintptr_t>(info->si_addr));
  void* frames[48];
  const int n = backtrace(frames, 48);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  if (g_callback) g_callback();
  std::_Exit(3);
}

#endif

}  // namespace

void install_crash_handler(const gekko::u8* guest_base, CrashCallback callback) {
  g_base = guest_base;
  g_callback = callback;
#ifdef _WIN32
  SetUnhandledExceptionFilter(on_exception);
#else
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_signal;
  sa.sa_flags = SA_SIGINFO;
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
#endif
}

}  // namespace os
