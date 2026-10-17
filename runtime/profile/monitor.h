// SPDX-License-Identifier: GPL-3.0-or-later
//
// Watches the process for a profile (profile/session.h), on its own thread:
//
//   * once a second, system.jsonl gets the CPU use of each thread (percent
//     of a core, by name: "main" renders, "guest N" run the game, "timer"
//     runs the hardware's timed events...), of the process and of the whole
//     computer (other programs competing for it), and the process's memory;
//   * on Windows, every busy thread's call stack is sampled (500 times a
//     second by default). Recompiled game functions are host functions named
//     after the game's (__imp__<name>_<address>), so the samples show the
//     game's code and OpenGP's alike.
//
// Samples go to cpu/run<N>/ (N: the session's run, see Session::run()):
//   threads.tsv   index, OS thread id, name (a thread renamed later gets a new line)
//   samples.bin   8 bytes each: u32 time in units of 100 us since the session started,
//                 u32 thread index << 24 | stack index
//   stacks.bin    the distinct stacks in index order: u16 depth, then depth u32 address
//                 indices, innermost first
//   symbols.tsv   address index, address (hex), function, offset (hex), module
//
// While sampling, a stopped thread's stack is copied and the walk done on
// the copy after it runs again: nothing that could need a lock the stopped
// thread holds (the heap, the loader) runs while it is stopped.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gekko/types.h"

namespace profile {

using namespace gekko;

class Session;

class Monitor {
 public:
  struct Options {
    int rate_hz = 500;   // stack samples a second per busy thread; 0: none
  };

  Monitor();
  ~Monitor();
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  void start(Session& session, const Options& options);
  void stop();

  // Where the game's threads spent the time between two session times (us):
  // the functions their samples were in ("self"), or that were on their
  // stacks ("inclusive", leaving out ones on nearly every stack), with each
  // one's share of the samples, the largest first. Samples of threads
  // blocked in a wait are left out. Empty without samples.
  struct Hot {
    std::string name;
    double share;
  };
  std::vector<Hot> hottest(u64 from_us, u64 to_us, std::size_t count, bool inclusive) const;

  // The whole computer's CPU use over the last second, 0-1 (-1 if unknown).
  double system_cpu() const { return system_cpu_.load(std::memory_order_relaxed); }

 private:
  struct State;
  void run();

  std::unique_ptr<State> state_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<double> system_cpu_{-1.0};
};

}  // namespace profile
