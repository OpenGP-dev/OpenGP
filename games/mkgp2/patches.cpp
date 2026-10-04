// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include "arcade.h"
#include "ppc_recomp_shared.h"

namespace guest {

// The arcade library's coin set-up: copies the coin settings from the system
// backup (SegaBoot's, normally set in its test menu) to 80690AA0. Its restart
// into the test menu is at 802878CC, and start-up reads the kept memory back
// at 802879C4.
PPC_FUNC(fn_80289B54) {
  __imp__fn_80289B54(ctx, base);
  arcade::on_coin_setup(base, 0x80690AA0u);
}

}  // namespace guest
