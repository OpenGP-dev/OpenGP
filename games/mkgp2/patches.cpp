// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include "arcade.h"
#include "attract_sound.h"
#include "camera_use.h"
#include "ppc_recomp_shared.h"

namespace guest {

[[maybe_unused]] static const bool kAttractHooked = attract_sound::declare_hook();

// The attract mode's constructor: its "quiet" flag is at +0x18. The demo
// scenes (80150C1C, the demo race, starts its music only when made with
// sound) are made from it at 8014E53C and 8014E998, and the flag is cleared
// when a pass ends (8014E368).
PPC_FUNC(fn_8014EDC0) {
  const gekko::u32 attract = ctx.gpr[3];
  __imp__fn_8014EDC0(ctx, base);
  attract_sound::on_attract_created(base, attract + 0x18);
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
}

// The wheel board's state, each frame: the camera's power.
PPC_FUNC(StrPcb_StateTick) {
  const gekko::u32 board = ctx.gpr[3];
  __imp__StrPcb_StateTick(ctx, base);
  camera_use::on_power_tick(base, board);
}

}  // namespace guest
