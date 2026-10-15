// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include "arcade.h"
#include "cabinet.h"
#include "camera_use.h"
#include "ppc_recomp_shared.h"
#include "sdk_hle.h"

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

}  // namespace guest
