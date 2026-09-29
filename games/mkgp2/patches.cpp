// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mario Kart Arcade GP 2 (GGPE02): replacements for recompiled functions.

#include "patches.h"

#include "ppc_recomp_shared.h"

namespace mkgp2 {
bool g_free_play = false;
}

namespace guest {

// The arcade library's coin set-up: copies the coin settings from the system
// backup (SegaBoot's, normally set in its test menu) to 80690AA0. Bytes
// 0xC-0xF are what a play costs, per coin slot; at 0 the game is free
// ("CREDIT(S) 0/0", "Step on gas pedal to start").
PPC_FUNC(fn_80289B54) {
  __imp__fn_80289B54(ctx, base);
  if (mkgp2::g_free_play) {
    for (unsigned i = 0; i < 4; ++i) base[0x80690AA0u + 0xC + i] = 0;
  }
}

}  // namespace guest
