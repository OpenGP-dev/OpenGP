// SPDX-License-Identifier: GPL-3.0-or-later
//
// Widescreen for both games: the culling frustum.
#pragma once

#include <atomic>

#include "gx/frontend.h"
#include "ppc/context.h"

namespace widescreen {

// For the games' frustum builder (GP 2's fn_80138184, GP's fn_8010948C), which
// makes a camera's culling frustum (CObj+0x3034) from its field of view (f1)
// and aspect (f2), for CObj_UpdatePerspParam, Scene3D_SetupProjection and
// Scene3D_Init. The games cull objects, and spread effects that fill the
// screen, against the 4:3 picture's frustum: in widescreen, objects in the
// side strips were missing and popped in at the 4:3 edges. Before the
// builder runs, this widens the aspect to the picture's width (the
// projection, made elsewhere, is unchanged).
inline void widen_frustum(ppc::Context& ctx) {
  if (const gekko::u32 side = gx::Frontend::g_widescreen_side.load(std::memory_order_relaxed)) {
    const float aspect = static_cast<float>(ctx.fpr[2].d0());
    ctx.fpr[2].set0(aspect * (static_cast<float>(640 + 2 * side) / 640.0f));
  }
}

}  // namespace widescreen
