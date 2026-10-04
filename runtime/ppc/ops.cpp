// SPDX-License-Identifier: GPL-3.0-or-later

#include "ppc/ops.h"

namespace ppc {

void reset_context(Context& ctx) {
  std::memset(&ctx, 0, sizeof(ctx));
  ctx.spr[spr::PVR] = GEKKO_PVR;
  // What the IPL leaves behind: paired singles, quantized load/store and the
  // write-gather pipe enabled; translation and FP enabled in the MSR.
  ctx.spr[spr::HID2] = HID2_LSQE | HID2_WPE | HID2_PSE;
  ctx.spr[spr::WPAR] = GATHER_PIPE_BASE & 0x1FFFFFFF;
  ctx.msr = 0x00002032;  // FP | IR | DR
}

namespace op {

// Locked cache <-> memory DMA, triggered by setting DMA_L[T]. Completes
// instantly (as in Dolphin).
void locked_cache_dma(Context& c, u8* base) {
  const u32 dmau = c.spr[spr::DMAU];
  const u32 dmal = c.spr[spr::DMAL];
  const u32 mem_addr = (dmau & ~31u) & 0x1FFFFFFF;  // physical
  const u32 lc_addr = (dmal & ~31u) & (LOCKED_CACHE_SIZE - 1);
  u32 lines = ((dmau & 31) << 2) | ((dmal >> 2) & 3);
  if (lines == 0) lines = 128;
  const u32 bytes = lines * 32;

  u8* ram = base + (MEM1_CACHED | mem_addr);
  u8* lc = base + LOCKED_CACHE_BASE + lc_addr;
  const u32 lc_room = LOCKED_CACHE_SIZE - lc_addr;
  const u32 n = bytes < lc_room ? bytes : lc_room;
  if (dmal & 0x10)
    std::memcpy(lc, ram, n);  // load: memory -> locked cache
  else
    std::memcpy(ram, lc, n);  // store: locked cache -> memory
  c.spr[spr::DMAL] &= ~2u;    // clear T
}

}  // namespace op
}  // namespace ppc
