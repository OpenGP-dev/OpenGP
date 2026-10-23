// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crashes on purpose, for tests/check_crash.cmake: installs the crash handler
// with a report file, then fails in the way asked for. The handler must write
// the report and end the process with exit code 3.
//
//   crash_probe REPORT fatal|null|abort|overflow

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "os/crash.h"

namespace {

volatile int* volatile g_null = nullptr;
volatile int g_limit = 1 << 30;

// Deep enough to overflow any thread's stack; not a loop the compiler can
// turn it into.
int recurse(int depth) {
  volatile char pad[4096];
  pad[0] = static_cast<char>(depth);
  if (depth > g_limit) return pad[0];
  return recurse(depth + 1) + pad[0];
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  // os::crashing() says when a report is under way, and only then.
  os::install_crash_handler(
      nullptr,
      [](std::FILE* out) { std::fprintf(out, "probe callback ran%s\n", os::crashing() ? ", crashing" : ""); },
      argv[1], "crash_probe");
  if (os::crashing()) return 2;
  const std::string how = argv[2];
  if (how == "fatal") os::crash_now("fatal: the probe gave up");
  if (how == "null") return *g_null;
  if (how == "abort") std::abort();
  if (how == "overflow") {
    std::thread([] {
      os::prepare_thread_for_crashes();
      recurse(0);
    }).join();
  }
  return 2;
}
