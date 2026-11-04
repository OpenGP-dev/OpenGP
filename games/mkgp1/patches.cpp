// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP (GGPE01): replacements for recompiled functions.

#include "arcade.h"
#include "cabinet.h"
#include "camera_use.h"
#include "ppc_recomp_shared.h"
#include "sdk_hle.h"
#include "widescreen.h"

namespace guest {

// The arcade library's coin set-up (the same code as GP 2's): copies the coin
// settings from the system backup to 80577740. Its restart into the test menu
// is at 8024BBC4, and start-up reads the kept memory back at 8024BCBC.
PPC_FUNC(fn_8024DEDC) {
  __imp__fn_8024DEDC(ctx, base);
  arcade::on_coin_setup(base, 0x80577740u);
}

// Start-up (GP 2's GameBoot_Init): reads the settings (804B3B40) from the
// backup, or sets their defaults (80079048), before the device check.
PPC_FUNC(fn_8007A5CC) {
  __imp__fn_8007A5CC(ctx, base);
  camera_use::on_settings_loaded(base, 0x804B3B40u);
  cabinet::on_settings_loaded(base, 0x804B3B40u);
}

// The media board's "this cabinet's address" (as in GP 2), with the
// cabinets' list at 8036B790.
PPC_FUNC(acModifyMyIPaddr) {
  cabinet::on_modify_address(base, 0x804B3B40u, 0x8036B790u, &ctx.gpr[3]);
  __imp__acModifyMyIPaddr(ctx, base);
}

// The LINK screen (as in GP 2), with its
// timer at 805B8958.
PPC_FUNC(BootPCBCheck_Init) {
  __imp__BootPCBCheck_Init(ctx, base);
  cabinet::on_link_screen_start(base, 0x805B8958u);
}

PPC_FUNC(fn_801B0BD4) {
  cabinet::on_link_screen_frame(base, 0x805B8958u);
  __imp__fn_801B0BD4(ctx, base);
}

// The wheel board's state, each frame: the camera's power (as in GP 2).
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
  static constexpr sdk_hle::DvdFs kFs{-0x4D38, -0x4D44, -0x4D40, -0x4D34, 0x8040F510u};
  sdk_hle::convert_path_to_entrynum(ctx, base, kFs, __imp__DVDConvertPathToEntrynum);
}

// The culling frustum builder (GP 2's fn_80138184, called from the same
// CObj_UpdatePerspParam and Scene3D_SetupProjection): widened in widescreen
// (widescreen.h).
PPC_FUNC(fn_8010948C) {
  widescreen::widen_frustum(ctx);
  __imp__fn_8010948C(ctx, base);
}

}  // namespace guest
