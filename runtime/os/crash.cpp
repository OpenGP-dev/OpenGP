// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/crash.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <exception>

#include "os/utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <csignal>
#else
#include <csignal>
#include <fcntl.h>
#include <initializer_list>
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define OPENGP_HAVE_BACKTRACE 1
#endif
#endif

namespace os {

namespace {

const gekko::u8* g_base = nullptr;
CrashCallback g_callback = nullptr;
char g_header[1024];
char g_report_utf8[1024];
std::atomic<bool> g_crashing{false};
#ifdef _WIN32
wchar_t g_report_path[1024];
HANDLE g_report = INVALID_HANDLE_VALUE;
// The time limit on a crash report: a thread made at start-up (one made in
// the crash could wait for ever on the loader lock), set going by the event.
HANDLE g_watchdog_start = nullptr;
std::atomic<bool> g_watchdog_off{false};
#else
int g_report = -1;
#endif

// Output with fixed buffers and the system's own calls only: the crash may
// have been inside the heap or stdio.
void write_all(const char* s, std::size_t n) {
#ifdef _WIN32
  DWORD written = 0;
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, s, static_cast<DWORD>(n), &written, nullptr);
  if (g_report != INVALID_HANDLE_VALUE) WriteFile(g_report, s, static_cast<DWORD>(n), &written, nullptr);
#else
  for (const int fd : {STDERR_FILENO, g_report}) {
    if (fd < 0) continue;
    for (std::size_t done = 0; done < n;) {
      const ssize_t w = ::write(fd, s + done, n - done);
      if (w <= 0) break;
      done += static_cast<std::size_t>(w);
    }
  }
#endif
}

void out(const char* format, ...) {
  char buf[1024];
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (n > 0) write_all(buf, std::min(static_cast<std::size_t>(n), sizeof(buf) - 1));
}

void describe_guest(std::uintptr_t addr) {
  const auto base = reinterpret_cast<std::uintptr_t>(g_base);
  if (g_base && addr >= base && addr - base < (std::uintptr_t{1} << 32))
    out("  (guest address %08llX)\n", static_cast<unsigned long long>(addr - base));
}

// Starts the report: once for the process (a second crashing thread waits
// for the first to finish), with the header and the time.
void begin_report() {
  if (g_crashing.exchange(true)) {
#ifdef _WIN32
    Sleep(30000);
#else
    sleep(30);
#endif
    std::_Exit(3);
  }
  // From here on, at most 10 seconds (a report can block on a lock the crash
  // left held): then the process ends anyway.
#ifdef _WIN32
  if (g_watchdog_start) SetEvent(g_watchdog_start);
#else
  std::signal(SIGALRM, SIG_DFL);
  alarm(10);
#endif
#ifdef _WIN32
  if (g_report_path[0])
    g_report = CreateFileW(g_report_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
#else
  if (g_report_utf8[0]) g_report = ::open(g_report_utf8, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
  const std::time_t now = std::time(nullptr);
  std::tm t{};
#ifdef _WIN32
  gmtime_s(&t, &now);
#else
  gmtime_r(&now, &t);
#endif
  out("\n[crash] %s(%04d-%02d-%02d %02d:%02d:%02d UTC)\n", g_header, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
      t.tm_hour, t.tm_min, t.tm_sec);
}

#ifdef _WIN32

DWORD WINAPI watchdog(void*) {
  WaitForSingleObject(g_watchdog_start, INFINITE);
  Sleep(10000);
  if (!g_watchdog_off) TerminateProcess(GetCurrentProcess(), 3);
  return 0;
}

void print_location(const void* p) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(p), &module)) {
    GetModuleFileNameA(module, name, sizeof(name));
    const char* base_name = name;
    for (const char* c = name; *c; ++c)
      if (*c == '\\' || *c == '/') base_name = c + 1;
    out("%s+0x%llx", base_name,
        static_cast<unsigned long long>(reinterpret_cast<const char*>(p) - reinterpret_cast<const char*>(module)));
  } else {
    out("%p", p);
  }
}

void print_backtrace() {
  void* frames[48];
  const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
  out("  backtrace:\n");
  for (USHORT i = 0; i < n; ++i) {
    out("    ");
    print_location(frames[i]);
    out("\n");
  }
}

#else

void print_backtrace() {
#ifdef OPENGP_HAVE_BACKTRACE
  void* frames[48];
  const int n = backtrace(frames, 48);
  out("  backtrace:\n");
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  if (g_report >= 0) backtrace_symbols_fd(frames, n, g_report);
#endif
}

#endif

// Ends the report: the callback's part (which may allocate, so it runs under
// a time limit), where the report is, and the exit.
[[noreturn]] void end_report() {
#ifdef _WIN32
  if (g_report != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(g_report);
    CloseHandle(g_report);
    g_report = INVALID_HANDLE_VALUE;
  }
#else
  if (g_report >= 0) {
    ::close(g_report);
    g_report = -1;
  }
#endif
  if (g_callback) {
    g_callback(stderr);
    if (g_report_utf8[0]) {
      if (std::FILE* f = fopen_utf8(g_report_utf8, "ab")) {
        g_callback(f);
        std::fclose(f);
      }
    }
  }
  if (g_report_utf8[0]) std::fprintf(stderr, "[crash] report written to %s\n", g_report_utf8);
  std::fflush(stdout);
  std::fflush(stderr);
#ifdef _WIN32
  // Started from Explorer, the console window closes with the process: say
  // so in a window too.
  DWORD pids[2];
  if (GetConsoleProcessList(pids, 2) <= 1 && !std::getenv("OPENGP_NO_CRASH_DIALOG")) {
    g_watchdog_off = true;  // the message stays until it is read
    wchar_t text[1200];
    std::swprintf(text, std::size(text), L"OpenGP has crashed.%ls%ls", g_report_path[0] ? L"\n\nThe crash report is in\n" : L"",
                  g_report_path);
    MessageBoxW(nullptr, text, L"OpenGP", MB_OK | MB_ICONERROR | MB_TOPMOST);
  }
#endif
  std::_Exit(3);
}

#ifdef _WIN32

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  begin_report();
  out("[crash] exception %08lX at ", r->ExceptionCode);
  print_location(r->ExceptionAddress);
  out(" (thread %lu)\n", GetCurrentThreadId());
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    const auto kind = r->ExceptionInformation[0];
    out("  %s of address %p\n", kind == 0 ? "read" : kind == 1 ? "write" : "execute",
        reinterpret_cast<void*>(r->ExceptionInformation[1]));
    describe_guest(r->ExceptionInformation[1]);
  }
  // The handler runs on the faulting thread's stack, so this backtrace
  // includes the frames that led to the fault.
  print_backtrace();
  end_report();
}

void on_abort(int) { crash_now("abort"); }
#endif

// An exception nothing caught: its message, then the report.
[[noreturn]] void on_terminate() {
  char reason[512] = "uncaught exception";
  try {
    if (const std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
  } catch (const std::exception& e) {
    std::snprintf(reason, sizeof(reason), "uncaught exception: %s", e.what());
  } catch (...) {
  }
  crash_now(reason);
}

#ifdef _WIN32

#else

void on_signal(int sig, siginfo_t* info, void*) {
  begin_report();
  out("[crash] signal %d (%s), address %p\n", sig, sig == SIGABRT ? "abort" : sig == SIGSEGV ? "segmentation fault" : "",
      info->si_addr);
  describe_guest(reinterpret_cast<std::uintptr_t>(info->si_addr));
  print_backtrace();
  end_report();
}

#endif

}  // namespace

void install_crash_handler(const gekko::u8* guest_base, CrashCallback callback, const std::string& report_path,
                           const std::string& header) {
  g_base = guest_base;
  g_callback = callback;
  std::snprintf(g_header, sizeof(g_header), "%s%s", header.c_str(), header.empty() ? "" : " ");
  std::snprintf(g_report_utf8, sizeof(g_report_utf8), "%s", report_path.size() < sizeof(g_report_utf8) ? report_path.c_str() : "");
#ifdef _WIN32
  g_report_path[0] = 0;
  if (g_report_utf8[0])
    MultiByteToWideChar(CP_UTF8, 0, g_report_utf8, -1, g_report_path, static_cast<int>(std::size(g_report_path)));
  if (!g_watchdog_start) {
    g_watchdog_start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, watchdog, nullptr, 0, nullptr)) CloseHandle(t);
  }
  SetUnhandledExceptionFilter(on_exception);
  // abort() (an uncaught exception, a failed assert) raises SIGABRT, which no
  // exception filter sees; and no "abort() has been called" dialog.
  std::signal(SIGABRT, on_abort);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
#ifdef OPENGP_HAVE_BACKTRACE
  void* warm[1];
  backtrace(warm, 1);  // its first call loads libgcc, which allocates: not in the handler
#endif
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_signal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
  prepare_thread_for_crashes();
}

void prepare_thread_for_crashes() {
  static thread_local bool done = false;
  if (done) return;
  done = true;
  std::set_terminate(on_terminate);  // per thread in Windows' C runtime
  constexpr std::size_t kSize = 64 * 1024;
#ifdef _WIN32
  // Stack kept back for the exception filter after a stack overflow.
  ULONG size = kSize;
  SetThreadStackGuarantee(&size);
#else
  // A stack of its own for the signal handler, for a stack overflow.
  stack_t ss{};
  ss.ss_sp = std::malloc(kSize);
  ss.ss_size = kSize;
  if (ss.ss_sp) sigaltstack(&ss, nullptr);
#endif
}

void crash_now(const char* reason) {
  begin_report();
  out("[crash] %s\n", reason);
  print_backtrace();
  end_report();
}

bool crashing() { return g_crashing.load(); }

}  // namespace os
