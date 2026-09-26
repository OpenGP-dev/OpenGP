// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/printf.h"

#include <bit>
#include <cstdio>
#include <cstring>

#include "ppc/memory.h"

namespace os {

namespace {

bool readable(u32 addr) {
  return (addr >= 0x80000000 && addr < 0x84000000) || (addr >= 0xC0000000 && addr < 0xC4000000);
}

}  // namespace

std::string read_guest_string(const u8* base, u32 addr, std::size_t max) {
  std::string s;
  while (s.size() < max && readable(addr)) {
    const char ch = static_cast<char>(base[addr]);
    if (!ch) break;
    s += ch;
    ++addr;
  }
  return s;
}

std::string format_guest(const ppc::Context& ctx, const u8* base, u32 fmt_addr, u32 first_gpr) {
  const std::string fmt = read_guest_string(base, fmt_addr);
  u32 next_gpr = first_gpr, next_fpr = 1;
  u32 stack = ctx.gpr[1] + 8;
  auto next_int = [&]() -> u32 {
    if (next_gpr <= 10) return ctx.gpr[next_gpr++];
    const u32 v = readable(stack) ? ppc::load_be<u32>(base + stack) : 0;
    stack += 4;
    return v;
  };
  auto next_double = [&]() -> double {
    if (next_fpr <= 8) return ctx.fpr[next_fpr++].d0();
    stack = (stack + 7) & ~7u;
    const u64 v = readable(stack) ? ppc::load_be<u64>(base + stack) : 0;
    stack += 8;
    return std::bit_cast<double>(v);
  };

  std::string out;
  for (std::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out += fmt[i];
      continue;
    }
    std::string spec = "%";
    ++i;
    while (i < fmt.size() && std::strchr("-+ #0123456789.", fmt[i])) spec += fmt[i++];
    bool long_long = false;
    while (i < fmt.size() && std::strchr("hlLqjzt", fmt[i])) {
      if (fmt[i] == 'l' && i + 1 < fmt.size() && fmt[i + 1] == 'l') long_long = true;
      ++i;
    }
    if (i >= fmt.size()) break;
    const char conv = fmt[i];
    char buf[512];
    switch (conv) {
      case 'd':
      case 'i':
        if (long_long) {
          next_gpr += (next_gpr & 1) == 0 ? 1 : 0;  // 64-bit values use an odd/even register pair
          const u64 hi = next_int(), lo = next_int();
          std::snprintf(buf, sizeof(buf), (spec + "lld").c_str(), static_cast<long long>((hi << 32) | lo));
        } else {
          std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int>(next_int()));
        }
        break;
      case 'u':
      case 'x':
      case 'X':
      case 'o':
        if (long_long) {
          next_gpr += (next_gpr & 1) == 0 ? 1 : 0;
          const u64 hi = next_int(), lo = next_int();
          std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), static_cast<unsigned long long>((hi << 32) | lo));
        } else {
          std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_int());
        }
        break;
      case 'c': std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), static_cast<int>(next_int())); break;
      case 'p': std::snprintf(buf, sizeof(buf), "0x%08X", next_int()); break;
      case 's':
        std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), read_guest_string(base, next_int()).c_str());
        break;
      case 'f':
      case 'F':
      case 'e':
      case 'E':
      case 'g':
      case 'G':
      case 'a':
      case 'A': std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), next_double()); break;
      case '%': std::snprintf(buf, sizeof(buf), "%%"); break;
      default: std::snprintf(buf, sizeof(buf), "%s%c", spec.c_str(), conv); break;
    }
    out += buf;
  }
  return out;
}

}  // namespace os
