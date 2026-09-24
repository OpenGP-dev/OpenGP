// SPDX-License-Identifier: GPL-3.0-or-later
//
// Guest memory. A 4 GiB host range is reserved so that a guest effective
// address maps to `base + addr`. MEM1 appears at both 0x80000000 (cached)
// and 0xC0000000 (uncached), backed by the same pages; the locked cache sits
// at 0xE0000000. Hardware registers (0xCC000000) are routed to hooks.
#pragma once

#include <bit>
#include <cstring>
#include <string>

#include "ppc/context.h"
#include "ppc/hooks.h"

namespace ppc {

inline constexpr u32 MEM1_CACHED = 0x80000000;
inline constexpr u32 MEM1_UNCACHED = 0xC0000000;
inline constexpr u32 MEM1_SIZE_GAMECUBE = 0x01800000;  // 24 MiB
inline constexpr u32 LOCKED_CACHE_BASE = 0xE0000000;
inline constexpr u32 LOCKED_CACHE_SIZE = 0x4000;  // 16 KiB
inline constexpr u32 IO_BASE = 0xCC000000;
inline constexpr u32 GATHER_PIPE_BASE = 0xCC008000;

class GuestMemory {
 public:
  GuestMemory() = default;
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  // Reserves the address space and maps MEM1 (mem1_size bytes, a multiple of
  // 64 KiB) and the locked cache. Returns false and fills `error` on failure.
  bool init(u32 mem1_size, std::string* error = nullptr);
  void shutdown();

  u8* base() const { return base_; }
  u32 mem1_size() const { return mem1_size_; }

  // True if [addr, addr+size) lies entirely in mapped RAM (either MEM1 view or
  // the locked cache).
  bool is_ram(u32 addr, u32 size = 1) const;

  // Host pointer for a guest address, or nullptr if not RAM.
  u8* pointer(u32 addr, u32 size = 1) const { return is_ram(addr, size) ? base_ + addr : nullptr; }

 private:
  u8* base_ = nullptr;
  u32 mem1_size_ = 0;
#ifdef _WIN32
  void* mem1_handle_ = nullptr;
#else
  int mem1_fd_ = -1;
#endif
};

// How a load/store's address is known to behave. Chosen by the recompiler per
// instruction; the interpreter always uses Checked.
enum class Mem {
  Ram,         // proven to be RAM: direct host access
  Mmio,        // proven to be a hardware register (0xCC000000 page)
  GatherPipe,  // proven to be the write-gather pipe (0xCC008000)
  Checked,     // unknown: test for the I/O page at runtime
};

inline bool is_io(u32 addr) { return (addr >> 16) == (IO_BASE >> 16); }
inline bool is_gather_pipe(u32 addr) { return (addr & 0xFFFFF000) == GATHER_PIPE_BASE; }

template <typename T>
inline T byteswap_value(T v) {
  if constexpr (sizeof(T) == 1) {
    return v;
  } else if constexpr (sizeof(T) == 2) {
    return static_cast<T>(__builtin_bswap16(static_cast<u16>(v)));
  } else if constexpr (sizeof(T) == 4) {
    return static_cast<T>(__builtin_bswap32(static_cast<u32>(v)));
  } else {
    return static_cast<T>(__builtin_bswap64(static_cast<u64>(v)));
  }
}

template <typename T>
inline T load_be(const u8* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return byteswap_value(v);
}

template <typename T>
inline void store_be(u8* p, T v) {
  v = byteswap_value(v);
  std::memcpy(p, &v, sizeof(T));
}

// I/O slow paths (memory.cpp).
u64 io_read(u32 addr, u32 size);
void io_write(u32 addr, u64 value, u32 size);

template <Mem K, typename T>
inline T read(u8* base, u32 addr) {
  if constexpr (K == Mem::Ram) {
    return load_be<T>(base + addr);
  } else if constexpr (K == Mem::Mmio || K == Mem::GatherPipe) {
    return static_cast<T>(io_read(addr, sizeof(T)));
  } else {
    if (is_io(addr)) [[unlikely]]
      return static_cast<T>(io_read(addr, sizeof(T)));
    return load_be<T>(base + addr);
  }
}

template <Mem K, typename T>
inline void write(u8* base, u32 addr, T value) {
  if constexpr (K == Mem::Ram) {
    store_be<T>(base + addr, value);
  } else if constexpr (K == Mem::Mmio || K == Mem::GatherPipe) {
    io_write(addr, static_cast<u64>(value), sizeof(T));
  } else {
    if (is_io(addr)) [[unlikely]] {
      io_write(addr, static_cast<u64>(value), sizeof(T));
      return;
    }
    store_be<T>(base + addr, value);
  }
}

}  // namespace ppc
