// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the host threads are doing, for hang reports (app/hang_watch.h): a
// short description and since when. Setting it is cheap enough to do a few
// times a frame; reading it is safe from any thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace os {

class Activity {
 public:
  // `what` must be a string literal; nullptr when there is nothing to report.
  void set(const char* what) {
    since_ns_.store(now_ns(), std::memory_order_relaxed);
    what_.store(what, std::memory_order_release);
  }

  // "<thread>: <what> for <seconds> s", or "" when not set.
  std::string describe(const char* thread) const {
    const char* what = what_.load(std::memory_order_acquire);
    if (!what) return {};
    const double seconds = static_cast<double>(now_ns() - since_ns_.load(std::memory_order_relaxed)) * 1e-9;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s: %s for %.1f s\n", thread, what, seconds);
    return buf;
  }

 private:
  static std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  std::atomic<const char*> what_{nullptr};
  std::atomic<std::int64_t> since_ns_{0};
};

// The main thread (the window, the renderer) and the game's thread when it
// waits on the host (for the renderer, for a readback of the picture).
inline Activity g_main_activity;
inline Activity g_guest_wait;

}  // namespace os
