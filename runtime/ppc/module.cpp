// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/module.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "ppc/memory.h"

namespace ppc {

void install_module(const RecompiledModule& module, u8* base) {
  g_functions.add(std::span<const FuncMapping>(module.functions, module.function_count));
  for (std::size_t i = 0; i < module.patch_count; ++i)
    store_be<u32>(base + module.patches[i].address, module.patches[i].value);
}

bool load_dol(const u8* data, std::size_t size, u8* base, u32* entry_point, std::string* error) {
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    return false;
  };
  if (size < 0x100) return fail("DOL too small");
  auto be32 = [&](std::size_t off) { return load_be<u32>(data + off); };
  auto in_ram = [](u32 addr, u32 len) {
    return (addr >= MEM1_CACHED && u64{addr} + len <= u64{MEM1_CACHED} + 0x04000000) ||
           (addr >= MEM1_UNCACHED && u64{addr} + len <= u64{MEM1_UNCACHED} + 0x04000000);
  };
  for (int i = 0; i < 18; ++i) {
    const u32 offset = be32(0x00 + 4 * i);
    const u32 address = be32(0x48 + 4 * i);
    const u32 len = be32(0x90 + 4 * i);
    if (len == 0) continue;
    if (u64{offset} + len > size) return fail("DOL section extends past end of file");
    if (!in_ram(address, len)) return fail("DOL section outside MEM1");
    std::memcpy(base + address, data + offset, len);
  }
  const u32 bss = be32(0xD8), bss_size = be32(0xDC);
  if (bss_size) {
    if (!in_ram(bss, bss_size)) return fail("DOL BSS outside MEM1");
    // The BSS range overlaps the small data sections in many DOLs; only clear
    // bytes that no section loaded.
    std::vector<std::pair<u32, u32>> loaded;
    for (int i = 0; i < 18; ++i) {
      const u32 address = be32(0x48 + 4 * i), len = be32(0x90 + 4 * i);
      if (len) loaded.emplace_back(address, address + len);
    }
    std::sort(loaded.begin(), loaded.end());
    u32 a = bss;
    const u32 end = bss + bss_size;
    for (const auto& [lo, hi] : loaded) {
      if (hi <= a) continue;
      if (lo >= end) break;
      if (lo > a) std::memset(base + a, 0, lo - a);
      a = std::max(a, hi);
    }
    if (a < end) std::memset(base + a, 0, end - a);
  }
  if (entry_point) *entry_point = be32(0xE0);
  return true;
}

}  // namespace ppc
