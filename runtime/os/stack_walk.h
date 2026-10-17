// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host call stacks: the calling thread's (for frame captures), and on Windows
// x64 a stopped thread's, walked from its registers with the unwind data
// every non-leaf function has (for the samplers in os/profiler.cpp and
// profile/monitor.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <execinfo.h>
#endif

namespace os {

// Fills `frames` with the calling thread's return addresses, innermost
// first, leaving out this call's own; returns how many.
inline std::size_t capture_stack(std::uint64_t* frames, std::size_t max) {
  void* buffer[64];
  if (max > 64) max = 64;
#ifdef _WIN32
  const std::size_t n = RtlCaptureStackBackTrace(1, static_cast<DWORD>(max), buffer, nullptr);
  for (std::size_t i = 0; i < n; ++i) frames[i] = reinterpret_cast<std::uint64_t>(buffer[i]);
  return n;
#else
  const int n = backtrace(buffer, static_cast<int>(max));
  std::size_t out = 0;
  for (int i = 1; i < n; ++i) frames[out++] = reinterpret_cast<std::uint64_t>(buffer[i]);
  return out;
#endif
}

#ifdef _WIN32

// Fills `frames` with the return addresses on the stack, innermost first
// (the first is where the thread is); returns how many.
inline std::size_t walk_stack(CONTEXT context, DWORD64* frames, std::size_t max) {
  std::size_t n = 0;
  while (n < max && context.Rip) {
    frames[n++] = context.Rip;
    DWORD64 image_base = 0;
    RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {  // leaf function: the return address is on top of the stack
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                     nullptr);
  }
  return n;
}

#endif

}  // namespace os
