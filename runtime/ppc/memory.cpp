// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/memory.h"

#include <cerrno>
#include <cstdio>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10: placeholder APIs
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000005  // NTDDI_WIN10_RS4 (1803)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ppc {

namespace {

constexpr u64 kReserveSize = u64{1} << 32;

void set_error(std::string* error, const char* what) {
  if (!error) return;
  char buf[256];
#ifdef _WIN32
  std::snprintf(buf, sizeof(buf), "%s (error %lu)", what, GetLastError());
#else
  std::snprintf(buf, sizeof(buf), "%s (errno %d)", what, errno);
#endif
  *error = buf;
}

}  // namespace

GuestMemory::~GuestMemory() { shutdown(); }

bool GuestMemory::is_ram(u32 addr, u32 size) const {
  const u64 end = u64{addr} + size;
  auto in = [&](u32 lo, u32 len) { return addr >= lo && end <= u64{lo} + len; };
  return in(MEM1_CACHED, mem1_size_) || in(MEM1_UNCACHED, mem1_size_) ||
         in(LOCKED_CACHE_BASE, LOCKED_CACHE_SIZE);
}

#ifndef _WIN32

bool GuestMemory::init(u32 mem1_size, std::string* error) {
  shutdown();
  mem1_size_ = mem1_size;
  g_mem1_size = mem1_size;

  void* reserved = mmap(nullptr, kReserveSize, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (reserved == MAP_FAILED) {
    set_error(error, "failed to reserve 4 GiB of address space");
    return false;
  }
  base_ = static_cast<u8*>(reserved);

#ifdef __linux__
  mem1_fd_ = memfd_create("opengp-mem1", MFD_CLOEXEC);
#else
  // Other POSIX systems: an unlinked shared-memory object.
  char name[64];
  std::snprintf(name, sizeof(name), "/opengp-mem1-%d", static_cast<int>(getpid()));
  mem1_fd_ = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (mem1_fd_ >= 0) shm_unlink(name);
#endif
  if (mem1_fd_ < 0 || ftruncate(mem1_fd_, mem1_size) != 0) {
    set_error(error, "failed to create MEM1 backing");
    shutdown();
    return false;
  }
  for (u32 view : {MEM1_CACHED, MEM1_UNCACHED}) {
    void* p = mmap(base_ + view, mem1_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                   mem1_fd_, 0);
    if (p == MAP_FAILED) {
      set_error(error, "failed to map MEM1 view");
      shutdown();
      return false;
    }
  }
  void* lc = mmap(base_ + LOCKED_CACHE_BASE, LOCKED_CACHE_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (lc == MAP_FAILED) {
    set_error(error, "failed to map the locked cache");
    shutdown();
    return false;
  }
  return true;
}

void GuestMemory::shutdown() {
  if (base_) munmap(base_, kReserveSize);
  if (mem1_fd_ >= 0) close(mem1_fd_);
  base_ = nullptr;
  mem1_fd_ = -1;
}

#else  // _WIN32

namespace {

using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                       MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);

struct Win32Api {
  VirtualAlloc2Fn virtual_alloc2 = nullptr;
  MapViewOfFile3Fn map_view_of_file3 = nullptr;
  Win32Api() {
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = LoadLibraryW(L"kernelbase.dll");
    if (!kb) return;
    // Go through void* so GCC's -Wcast-function-type accepts the FARPROC cast.
    virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(
        reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")));
    map_view_of_file3 = reinterpret_cast<MapViewOfFile3Fn>(
        reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3")));
  }
};

const Win32Api& api() {
  static const Win32Api a;
  return a;
}

}  // namespace

// Windows maps views into a reservation through "placeholders": reserve the
// whole range as one placeholder, split it at the view boundaries, then
// replace each piece with a mapped view or committed memory.
bool GuestMemory::init(u32 mem1_size, std::string* error) {
  shutdown();
  mem1_size_ = mem1_size;
  g_mem1_size = mem1_size;
  const Win32Api& w = api();
  if (!w.virtual_alloc2 || !w.map_view_of_file3) {
    set_error(error, "VirtualAlloc2/MapViewOfFile3 unavailable (Windows 10 1803+ required)");
    return false;
  }

  base_ = static_cast<u8*>(w.virtual_alloc2(nullptr, nullptr, kReserveSize,
                                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                            nullptr, 0));
  if (!base_) {
    set_error(error, "failed to reserve 4 GiB of address space");
    return false;
  }

  auto split = [&](u64 offset, u64 size) {
    return VirtualFree(base_ + offset, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
  };
  // Carve out exactly the placeholders we will replace.
  const u64 regions[][2] = {
      {MEM1_CACHED, mem1_size},
      {MEM1_UNCACHED, mem1_size},
      {LOCKED_CACHE_BASE, LOCKED_CACHE_SIZE},
  };
  // Regions are in ascending order, so the placeholder that contains the next
  // region always starts at `ph_start` and runs to the end of the reservation.
  // A split must name the start of the placeholder being split.
  u64 ph_start = 0;
  for (const auto& r : regions) {
    if (r[0] > ph_start && !split(ph_start, r[0] - ph_start)) {
      set_error(error, "failed to split placeholder");
      shutdown();
      return false;
    }
    if (r[0] + r[1] < kReserveSize && !split(r[0], r[1])) {
      set_error(error, "failed to split placeholder");
      shutdown();
      return false;
    }
    ph_start = r[0] + r[1];
  }

  mem1_handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mem1_size,
                                    nullptr);
  if (!mem1_handle_) {
    set_error(error, "failed to create MEM1 backing");
    shutdown();
    return false;
  }
  for (u32 view : {MEM1_CACHED, MEM1_UNCACHED}) {
    void* p = w.map_view_of_file3(mem1_handle_, GetCurrentProcess(), base_ + view, 0, mem1_size,
                                  MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (!p) {
      set_error(error, "failed to map MEM1 view");
      shutdown();
      return false;
    }
  }
  void* lc = w.virtual_alloc2(nullptr, base_ + LOCKED_CACHE_BASE, LOCKED_CACHE_SIZE,
                              MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
  if (!lc || !VirtualAlloc(lc, LOCKED_CACHE_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
    set_error(error, "failed to map the locked cache");
    shutdown();
    return false;
  }
  return true;
}

void GuestMemory::shutdown() {
  if (base_) {
    for (u32 view : {MEM1_CACHED, MEM1_UNCACHED}) UnmapViewOfFile(base_ + view);
    VirtualFree(base_ + LOCKED_CACHE_BASE, 0, MEM_RELEASE);
    // Release every remaining placeholder in the range.
    u8* p = base_;
    while (p < base_ + kReserveSize) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.RegionSize == 0) break;
      if (mbi.State == MEM_RESERVE) VirtualFree(mbi.AllocationBase, 0, MEM_RELEASE);
      p = static_cast<u8*>(mbi.BaseAddress) + mbi.RegionSize;
    }
  }
  if (mem1_handle_) CloseHandle(mem1_handle_);
  base_ = nullptr;
  mem1_handle_ = nullptr;
}

#endif  // _WIN32

u64 io_read(u32 addr, u32 size) {
  if (size == 8) {
    const u64 hi = g_hooks.mmio_read(addr, 4);
    const u64 lo = g_hooks.mmio_read(addr + 4, 4);
    return (hi << 32) | lo;
  }
  return g_hooks.mmio_read(addr, size);
}

void io_write(u32 addr, u64 value, u32 size) {
  if (is_gather_pipe(addr)) {
    u8 bytes[8];
    for (u32 i = 0; i < size; ++i) bytes[i] = static_cast<u8>(value >> (8 * (size - 1 - i)));
    g_hooks.gather_pipe_write(bytes, size);
    return;
  }
  if (size == 8) {
    g_hooks.mmio_write(addr, static_cast<u32>(value >> 32), 4);
    g_hooks.mmio_write(addr + 4, static_cast<u32>(value), 4);
    return;
  }
  g_hooks.mmio_write(addr, static_cast<u32>(value), size);
}

}  // namespace ppc
