// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/printf.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>

#include "ppc/memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(OPENGP_HAVE_ICONV)
#include <iconv.h>
#endif

namespace os {

namespace {

// In MEM1 as mapped (48 MiB for GP 2, 24 MiB for GP): through the cached or
// the uncached view.
bool readable(u32 addr, u32 bytes = 1) {
  const u64 size = ppc::g_mem1_size ? ppc::g_mem1_size : 24u << 20;
  return u64{addr - 0x80000000u} + bytes <= size || u64{addr - 0xC0000000u} + bytes <= size;
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
    const u32 v = readable(stack, 4) ? ppc::load_be<u32>(base + stack) : 0;
    stack += 4;
    return v;
  };
  auto next_double = [&]() -> double {
    if (next_fpr <= 8) return ctx.fpr[next_fpr++].d0();
    stack = (stack + 7) & ~7u;
    const u64 v = readable(stack, 8) ? ppc::load_be<u64>(base + stack) : 0;
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

std::string sjis_to_utf8(const std::string& text) {
  if (std::all_of(text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 0x80; }))
    return text;
#ifdef _WIN32
  // Code page 932 is Microsoft's Shift JIS.
  const int size = static_cast<int>(text.size());
  const int wide = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
  if (wide <= 0) return text;
  std::wstring w(static_cast<std::size_t>(wide), L'\0');
  MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, text.data(), size, w.data(), wide);
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), n, nullptr, nullptr);
  return out;
#elif defined(OPENGP_HAVE_ICONV)
  iconv_t cd = iconv_open("UTF-8", "CP932");
  if (cd == reinterpret_cast<iconv_t>(-1)) return text;
  std::string in = text;
  // A byte of Shift JIS (half-width katakana) is 3 bytes of UTF-8.
  std::string out(text.size() * 3, '\0');
  char* src = in.data();
  char* dst = out.data();
  std::size_t src_left = in.size(), dst_left = out.size();
  const bool ok = iconv(cd, &src, &src_left, &dst, &dst_left) != static_cast<std::size_t>(-1);
  iconv_close(cd);
  if (!ok) return text;
  out.resize(out.size() - dst_left);
  return out;
#else
  return text;
#endif
}

void use_utf8_console() {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
}

namespace {
void (*g_console_close)() = nullptr;
}

void on_console_close(void (*callback)()) {
  g_console_close = callback;
#ifdef _WIN32
  SetConsoleCtrlHandler(
      [](DWORD type) -> BOOL {
        if (type != CTRL_CLOSE_EVENT && type != CTRL_LOGOFF_EVENT && type != CTRL_SHUTDOWN_EVENT) return FALSE;
        if (g_console_close) g_console_close();
        Sleep(5000);  // the callback ends the process before this returns
        return TRUE;
      },
      TRUE);
#endif
}

}  // namespace os
