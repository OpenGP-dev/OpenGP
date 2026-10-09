// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP (GGPE01): replacements for recompiled functions.

#include "arcade.h"
#include "camera_use.h"
#include "ppc_recomp_shared.h"

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
}

// The wheel board's state, each frame: the camera's power (as in GP 2).
PPC_FUNC(StrPcb_StateTick) {
  const gekko::u32 board = ctx.gpr[3];
  __imp__StrPcb_StateTick(ctx, base);
  camera_use::on_power_tick(base, board);
}

}  // namespace guest
