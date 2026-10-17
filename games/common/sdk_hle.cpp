// SPDX-License-Identifier: GPL-3.0-or-later
//
// Replacements for GameCube SDK functions, shared by the games. Each one
// overrides the recompiled (weak) function of the same name; see
// runtime/os/threads.h for why the context functions are replaced.
//
// Compiled into each game's executable against that game's generated header.

#include "sdk_hle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "os/printf.h"
#include "os/reboot.h"
#include "os/threads.h"
#include "ppc/memory.h"
#include "ppc_recomp_shared.h"

namespace {

// The memory the game asked to keep across a restart (OSSetSaveRegion).
gekko::u32 g_save_start = 0, g_save_end = 0;

void write_be32(gekko::u8* base, gekko::u32 address, gekko::u32 value) {
  for (int i = 0; i < 4; ++i) base[address + i] = static_cast<gekko::u8>(value >> (24 - 8 * i));
}

// [address, address + size) in MEM1 (cached or uncached view), or nullptr:
// hardware registers, the locked cache and unmapped addresses aren't.
gekko::u8* ram(gekko::u8* base, gekko::u32 address, gekko::u32 size) {
  const gekko::u32 top = address >> 28;
  if (top != 0x8 && top != 0xC) return nullptr;
  if (gekko::u64{address & 0x0FFFFFFF} + size > ppc::g_mem1_size) return nullptr;
  return base + address;
}

}  // namespace

namespace guest {

// OSReport(const char* fmt, ...): print to the host console.
PPC_FUNC(OSReport) {
  std::string text = os::format_guest(ctx, base, ctx.gpr[3], 4);
  if (text.empty() || text.back() != '\n') text += '\n';
  std::fputs(("[OSReport] " + os::sjis_to_utf8(text)).c_str(), stdout);
  std::fflush(stdout);
}

// OSSaveContext runs as recompiled code; the thread manager only needs to
// know which context the calling thread saved.
PPC_FUNC(OSSaveContext) {
  os::g_threads->on_save_context(ctx, ctx.gpr[3]);
  __imp__OSSaveContext(ctx, base);
}

// OSLoadContext switches to another guest thread (or returns from an
// exception); see os::ThreadManager::load_context.
PPC_FUNC(OSLoadContext) { os::g_threads->load_context(ctx, ctx.gpr[3]); }

// Mid-function hook on SelectThread's idle loop (recomp.toml): wait for an
// interrupt instead of spinning a host core.
PPC_HOOK(OSIdleWait) { os::g_threads->idle(ctx); }

// OSSetSaveRegion(start, end): memory to keep across a restart.
PPC_FUNC(OSSetSaveRegion) {
  g_save_start = ctx.gpr[3];
  g_save_end = ctx.gpr[4];
  __imp__OSSetSaveRegion(ctx, base);
}

// OSResetSystem(reset, resetCode, forceMenu): the launcher restarts the
// process (see os/reboot.h), keeping the save region for a restart
// (OS_RESET_RESTART); hot resets and shutdowns boot afresh.
PPC_FUNC(OSResetSystem) {
  os::Reboot r;
  r.restart = ctx.gpr[3] == 0;
  r.reset_code = ctx.gpr[4];
  constexpr gekko::u32 kRam = 0x80000000, kMaxRegion = 0x01000000;
  const gekko::u32 start = g_save_start | kRam, end = g_save_end | kRam;
  if (r.restart && g_save_end && start < end && end - start <= kMaxRegion) {
    r.region_start = start;
    r.region_end = end;
    r.region.assign(base + start, base + end);
  }
  os::reboot(r);
}

// __OSGetExecParams(params): OSInit asks how it was booted. After a restart,
// the reset code and the memory kept come from the previous run (os/reboot.h);
// the memory is put back now, before OSInit clears the rest of the arena.
// (symbols.txt names it without the leading underscores, which the
// recompiler would otherwise have to rename.)
PPC_FUNC(OSGetExecParams) {
  const os::Reboot* r = os::g_previous_boot;
  if (!r || !r->restart) {
    __imp__OSGetExecParams(ctx, base);
    return;
  }
  std::fprintf(stderr, "[os] booting after a restart (reset code %08X, %zu bytes kept)\n", r->reset_code,
               r->region.size());
  // OSExecParams: valid, restartCode, bootDol, regionStart, regionEnd, argsUseDefault, argsAddr.
  const gekko::u32 p = ctx.gpr[3];
  const gekko::u32 fields[7] = {1, r->reset_code, 0, r->region_start, r->region_end, 1, 0};
  for (int i = 0; i < 7; ++i) write_be32(base, p + 4 * i, fields[i]);
  if (!r->region.empty()) std::copy(r->region.begin(), r->region.end(), base + r->region_start);
}

// memcpy(dst, src, n): the game's is a byte loop with memmove's rule for
// overlaps (forward when src >= dst), and recompiled it costs a few
// nanoseconds a byte: loading screens copy megabytes with it. Copies within
// MEM1 are made natively; the rest (and the odd overlap of the cached and
// uncached views, where the byte loop's direction differs from memmove's)
// go through the original.
PPC_FUNC(memcpy) {
  const gekko::u32 dst = ctx.gpr[3], src = ctx.gpr[4], n = ctx.gpr[5];
  gekko::u8* d = ram(base, dst, n);
  const gekko::u8* s = ram(base, src, n);
  const gekko::u32 pd = dst & 0x0FFFFFFF, ps = src & 0x0FFFFFFF;
  const bool overlap = pd < ps + n && ps < pd + n;
  if (n == 0 || !d || !s || (overlap && (src >= dst) != (ps >= pd))) {
    __imp__memcpy(ctx, base);
    return;
  }
  std::memmove(d, s, n);  // r3 (the result) is still dst
}

// memset(dst, c, n), natively for MEM1, as memcpy.
PPC_FUNC(memset) {
  gekko::u8* d = ram(base, ctx.gpr[3], ctx.gpr[5]);
  if (ctx.gpr[5] == 0 || !d) {
    __imp__memset(ctx, base);
    return;
  }
  std::memset(d, static_cast<int>(ctx.gpr[4] & 0xFF), ctx.gpr[5]);
}

// RealMode(func) runs `func` with address translation off. The SDK only uses
// it for Config24MB/Config48MB, which reprogram the BATs for the memory size;
// the memory map is fixed here, so there is nothing to do.
PPC_FUNC(RealMode) {}

}  // namespace guest

namespace sdk_hle {

void convert_path_to_entrynum(ppc::Context& ctx, ppc::u8* base, const DvdFs& fs, GuestFunc original) {
  using gekko::u32;
  using gekko::u8;
  bool ok = true;  // every read was in MEM1 (else: the original, which reads as the game would)
  auto byte = [&](u32 address) -> u8 {
    const u8* p = ram(base, address, 1);
    if (!p) ok = false;
    return p ? *p : 0;
  };
  auto word = [&](u32 address) -> u32 {
    const u8* p = ram(base, address, 4);
    if (!p) ok = false;
    return p ? (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3] : 0;
  };
  const u32 sda = ctx.gpr[13];
  const u8* table = ram(base, fs.tolower_table, 256);
  if (!table || word(sda + static_cast<u32>(fs.long_file_names)) == 0) {  // 8.3 names: the checks
    original(ctx, base);
    return;
  }
  const u32 entries = word(sda + static_cast<u32>(fs.fst_start));
  const u32 strings = word(sda + static_cast<u32>(fs.fst_string_start));
  // tolower(), as the game calls it: with the character sign-extended, so
  // 0xFF is EOF (-1).
  auto lower = [&](u8 c) { return c == 0xFF ? -1 : static_cast<int>(table[c]); };

  auto lookup = [&]() -> u32 {
    u32 p = ctx.gpr[3];
    u32 dir = word(sda + static_cast<u32>(fs.current_directory));
    for (;;) {
      if (!ok) return 0;
      const u8 c = byte(p);
      if (c == 0) return dir;
      if (c == '/') {  // from the root
        dir = 0;
        ++p;
        continue;
      }
      if (c == '.') {
        const u8 c1 = byte(p + 1);
        if (c1 == '.') {
          const u8 c2 = byte(p + 2);
          if (c2 == '/') {  // the parent
            dir = word(entries + 12 * dir + 4);
            p += 3;
            continue;
          }
          if (c2 == 0) return word(entries + 12 * dir + 4);
        } else if (c1 == '/') {
          p += 2;
          continue;
        } else if (c1 == 0) {
          return dir;
        }
      }
      // The next name in the path, and whether a directory must match it.
      u32 end = p;
      while (ok && byte(end) != 0 && byte(end) != '/') ++end;
      const bool want_dir = byte(end) != 0;
      // The directory's entries; a subdirectory's are skipped as a whole.
      const u32 dir_end = word(entries + 12 * dir + 8);
      u32 i = dir + 1;
      for (; ok && i < dir_end;) {
        const u32 w0 = word(entries + 12 * i);
        const bool is_dir = (w0 & 0xFF000000) != 0;
        if (is_dir || !want_dir) {
          u32 s = strings + (w0 & 0x00FFFFFF), q = p;
          bool same = true;
          for (u8 sc; ok && (sc = byte(s)) != 0; ++s, ++q) {
            if (lower(byte(q)) != lower(sc)) {
              same = false;
              break;
            }
          }
          if (same) {
            const u8 qc = byte(q);
            same = qc == '/' || qc == 0;
          }
          if (same) break;
        }
        i = is_dir ? word(entries + 12 * i + 8) : i + 1;
      }
      if (!ok) return 0;
      if (i >= dir_end) return 0xFFFFFFFF;  // -1: not found
      if (!want_dir) return i;
      dir = i;
      p = end + 1;
    }
  };

  const u32 result = lookup();
  if (!ok) {
    original(ctx, base);
    return;
  }
  // OPENGP_CHECK_HLE=1: run the original too, and say if they differ.
  static const bool check = std::getenv("OPENGP_CHECK_HLE") != nullptr;
  if (check) {
    ppc::Context copy = ctx;
    original(copy, base);
    if (copy.gpr[3] != result) {
      std::fprintf(stderr, "[hle] DVDConvertPathToEntrynum(%s): %d natively, %d recompiled\n",
                   reinterpret_cast<const char*>(ram(base, ctx.gpr[3], 1)), static_cast<int>(result),
                   static_cast<int>(copy.gpr[3]));
    }
  }
  ctx.gpr[3] = result;
}

}  // namespace sdk_hle
