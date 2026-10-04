// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP (GGPE01): replacements for recompiled functions.

#include "arcade.h"
#include "ppc_recomp_shared.h"

namespace guest {

// The arcade library's coin set-up (the same code as GP 2's): copies the coin
// settings from the system backup to 80577740. Its restart into the test menu
// is at 8024BBC4, and start-up reads the kept memory back at 8024BCBC.
PPC_FUNC(fn_8024DEDC) {
  __imp__fn_8024DEDC(ctx, base);
  arcade::on_coin_setup(base, 0x80577740u);
}

}  // namespace guest
