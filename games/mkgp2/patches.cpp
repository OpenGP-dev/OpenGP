// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include <chrono>

#include "arcade.h"
#include "cabinet.h"
#include "camera_use.h"
#include "link_pace.h"
#include "ppc_recomp_shared.h"
#include "sdk_hle.h"
#include "widescreen.h"

namespace guest {

// The attract mode's constructor. The game plays the attract mode's first
// time round without sound: the object starts with a "quiet" flag (+0x18)
// that the scenes it makes copy (the promo at 8014E998, whose 80150C1C starts
// its music only when made with sound, and the instructions at 8014E53C),
// and clears it when a round ends (8014E368). OpenGP clears it at once, so
// every round has sound.
PPC_FUNC(fn_8014EDC0) {
  const gekko::u32 attract = ctx.gpr[3];
  __imp__fn_8014EDC0(ctx, base);
  base[attract + 0x18] = 0;
}

// The arcade library's coin set-up: copies the coin settings from the system
// backup (SegaBoot's, normally set in its test menu) to 80690AA0. Its restart
// into the test menu is at 802878CC, and start-up reads the kept memory back
// at 802879C4.
PPC_FUNC(fn_80289B54) {
  __imp__fn_80289B54(ctx, base);
  arcade::on_coin_setup(base, 0x80690AA0u);
}

// Start-up: reads the settings (805986C0) from the backup, or sets their
// defaults, before the device check.
PPC_FUNC(GameBoot_Init) {
  __imp__GameBoot_Init(ctx, base);
  camera_use::on_settings_loaded(base, 0x805986C0u);
  cabinet::on_settings_loaded(base, 0x805986C0u);
}

// The media board's "this cabinet's address" (acModifyMyIPaddr), which
// start-up calls with its address from the cabinets' list (803FD910).
PPC_FUNC(acModifyMyIPaddr) {
  cabinet::on_modify_address(base, 0x805986C0u, 0x803FD910u, &ctx.gpr[3]);
  __imp__acModifyMyIPaddr(ctx, base);
}

// The LINK screen, where the game looks for the other cabinets as it starts:
// its timer is at 806D0E1C (see cabinet.h).
PPC_FUNC(BootPCBCheck_Init) {
  __imp__BootPCBCheck_Init(ctx, base);
  cabinet::on_link_screen_start(base, 0x806D0E1Cu);
}

PPC_FUNC(BootPCBCheck_Poll) {
  cabinet::on_link_screen_frame(base, 0x806D0E1Cu);
  __imp__BootPCBCheck_Poll(ctx, base);
}

// The wheel board's state, each frame: the camera's power.
PPC_FUNC(StrPcb_StateTick) {
  const gekko::u32 board = ctx.gpr[3];
  __imp__StrPcb_StateTick(ctx, base);
  camera_use::on_power_tick(base, board);
}

// The SDK's path lookup, natively (sdk_hle.h): the game opens files by name
// in the middle of loading screens and races, and the recompiled lookup,
// which scans every file's name calling tolower() per character, stalled
// them for tens of milliseconds.
PPC_FUNC(DVDConvertPathToEntrynum) {
  static constexpr sdk_hle::DvdFs kFs{-0x4EB8, -0x4EC4, -0x4EC0, -0x4EB4, 0x804F4738u};
  sdk_hle::convert_path_to_entrynum(ctx, base, kFs, __imp__DVDConvertPathToEntrynum);
}

// Keeping linked cabinets in step (link_pace.h). PCBComm_Process(1, 1) is
// each frame's link exchange; VSMode_FrameUpdate's queue check reads each
// cabinet's frame entry with RingBuffer_GetKartSlot (returning to 8009DD08)
// and its queue at +0x22, and while one is 2 or more it waits a retrace and
// receives again (PCBComm_Process(1, 0), returning to 8009DD70), up to 4
// times: a frozen frame.
namespace {
constexpr link_pace::Layout kLinkLayout{
    .scene = 0x806CED74u,
    .race_first = 0x25,
    .race_last = 0x2D,
    .link_active = 0x806D0DF0u,
    .host_flag = 0x806D0F0Cu,
    .ring_read = 0x806D0DFCu,
    .ring_write = 0x806D0E00u,
    .ring_mask = 63,
};
}  // namespace

PPC_FUNC(PCBComm_Process) {
  const gekko::u32 receive = ctx.gpr[3], send = ctx.gpr[4], from = ctx.lr;
  const auto start = std::chrono::steady_clock::now();
  __imp__PCBComm_Process(ctx, base);
  if (from == 0x8009DD70u) link_pace::on_freeze();
  if (receive == 1 && send == 1) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    link_pace::on_frame(base, kLinkLayout, static_cast<gekko::u64>(ns));
  }
}

PPC_FUNC(RingBuffer_GetKartSlot) {
  const int cabinet = static_cast<int>(ctx.gpr[3]);
  const gekko::u32 from = ctx.lr;
  __imp__RingBuffer_GetKartSlot(ctx, base);
  if (from == 0x8009DD08u && ctx.gpr[3] && base[kLinkLayout.host_flag] == 1)
    link_pace::on_queue_check(base, ctx.gpr[3], 0x22, cabinet);
}

// The culling frustum builder: widened in widescreen (widescreen.h).
PPC_FUNC(fn_80138184) {
  widescreen::widen_frustum(ctx);
  __imp__fn_80138184(ctx, base);
}

}  // namespace guest
