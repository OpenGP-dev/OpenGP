// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on Dolphin's Boot_BS2Emu.cpp (CBoot::EmulatedBS2_GC, SetupGCMemory,
// RunApploader), Copyright Dolphin Emulator Project, GPL-2.0-or-later.

#include "os/boot.h"

#include <ctime>
#include <vector>

#include "ppc/interpreter.h"
#include "ppc/memory.h"
#include "ppc/ops.h"
#include "ppc/timebase.h"

namespace os {

namespace {

using ppc::load_be;
using ppc::store_be;

constexpr u32 kApploaderOffset = 0x2440;  // on disc
constexpr u32 kApploaderLoad = 0x81200000;
constexpr u32 kApploaderFuncs = 0x80003100;  // where the entry writes init/main/close pointers
constexpr u32 kApploaderReport = 0x81300000;  // report callback (a blr)
constexpr u32 kApploaderArgs = 0x81300004;    // main's three out-parameters
constexpr u32 kConsoleTypeLatestDevkit = 0x10000006;

// Runs one apploader function in the interpreter until it returns.
void run_function(ppc::Context& ctx, u8* base, u32 address) {
  ctx.lr = 0;  // return sentinel
  ppc::interpret(ctx, base, address);
}

}  // namespace

u64 boot_time_base() { return ppc::local_seconds_since_2000() * ppc::kTimebaseHz; }

bool boot_disc(const disc::Image& disc, u8* base, u32 mem1_size, ppc::Context& ctx, BootResult* result,
               std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };

  // CPU state the IPL leaves (Dolphin SetupMSR / SetupHID; BATs are implicit).
  ppc::reset_context(ctx);

  // Low-memory boot globals (Dolphin SetupGCMemory). The first 0x20 bytes of
  // the disc header (game ID, maker, disc number, version, streaming) go to 0x80000000.
  u8 header[0x20];
  disc.read(0, header, sizeof(header));
  std::memcpy(base + 0x80000000, header, sizeof(header));
  store_be<u32>(base + 0x80000020, 0x0D15EA5E);  // booted from the boot ROM
  store_be<u32>(base + 0x80000028, mem1_size);   // physical memory size
  store_be<u32>(base + 0x8000002C, kConsoleTypeLatestDevkit);
  store_be<u32>(base + 0x800000CC, 0);            // video mode: NTSC (Triforce is always NTSC)
  store_be<u32>(base + 0x800000D0, 0x01000000);   // ARAM size: 16 MiB
  store_be<u32>(base + 0x800000F8, 0x09A7EC80);   // bus clock: 162 MHz
  store_be<u32>(base + 0x800000FC, 0x1CF7C580);   // CPU clock: 486 MHz
  store_be<u32>(base + 0x80000300, 0x4C000064);   // default DSI handler: rfi
  store_be<u32>(base + 0x80000800, 0x4C000064);   // default FPU handler: rfi
  store_be<u32>(base + 0x80000C00, 0x4C000064);   // default syscall handler: rfi
  // System time = time base + this: none, as the time base already has the
  // date (see boot_time_base()).
  store_be<u64>(base + 0x800030D8, 0);

  // Registers the IPL leaves for the apploader (NTSC values).
  ctx.gpr[1] = 0x81566550;
  ctx.gpr[2] = 0x81465CC0;
  ctx.gpr[13] = 0x81465320;

  // Load the apploader: header at 0x2440 (entry 0x10, size 0x14, trailer 0x18), code at +0x20.
  const u32 entry = disc.read_u32(kApploaderOffset + 0x10);
  const u32 size = disc.read_u32(kApploaderOffset + 0x14);
  const u32 trailer = disc.read_u32(kApploaderOffset + 0x18);
  if (entry == 0xFFFFFFFF || size + trailer > 0x100000 || (entry >> 28) != 0x8)
    return fail("invalid apploader header (corrupt image?)");
  std::vector<u8> apploader(size + trailer);
  disc.read(kApploaderOffset + 0x20, apploader.data(), apploader.size());
  std::memcpy(base + kApploaderLoad, apploader.data(), apploader.size());

  // The apploader is plain PowerPC code; run it in the interpreter the way
  // the IPL calls it: entry() fills in pointers to init/main/close.
  ctx.gpr[3] = kApploaderFuncs + 0;
  ctx.gpr[4] = kApploaderFuncs + 4;
  ctx.gpr[5] = kApploaderFuncs + 8;
  run_function(ctx, base, entry);
  const u32 app_init = load_be<u32>(base + kApploaderFuncs + 0);
  const u32 app_main = load_be<u32>(base + kApploaderFuncs + 4);
  const u32 app_close = load_be<u32>(base + kApploaderFuncs + 8);

  // init(report): the report callback is a blr.
  store_be<u32>(base + kApploaderReport, 0x4E800020);
  ctx.gpr[3] = kApploaderReport;
  run_function(ctx, base, app_init);

  // main(&dst, &length, &offset) returns 1 while there is something to read.
  for (int guard = 0; guard < 4096; ++guard) {
    ctx.gpr[3] = kApploaderArgs + 0;
    ctx.gpr[4] = kApploaderArgs + 4;
    ctx.gpr[5] = kApploaderArgs + 8;
    run_function(ctx, base, app_main);
    if (ctx.gpr[3] == 0) break;
    const u32 dst = load_be<u32>(base + kApploaderArgs + 0);
    const u32 length = load_be<u32>(base + kApploaderArgs + 4);
    const u32 offset = load_be<u32>(base + kApploaderArgs + 8);
    u8* host = ppc::GuestMemory::host_pointer(base, dst, mem1_size, length);
    if (!host) return fail("apploader asked to load outside RAM");
    disc.read(offset, host, length);
  }

  run_function(ctx, base, app_close);
  if (result) {
    result->entry_point = ctx.gpr[3];
    result->fst_address = load_be<u32>(base + 0x80000038);
    result->arena_lo = load_be<u32>(base + 0x80000030);
    result->arena_hi = load_be<u32>(base + 0x80000034);
    result->bi2_address = load_be<u32>(base + 0x800000F4);
  }
  if ((ctx.gpr[3] >> 28) != 0x8) return fail("apploader returned an invalid entry point");
  return true;
}

}  // namespace os
