// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keeping linked cabinets in step (GP 2's Versus races).
//
// The link is a host-relay lockstep: every cabinet sends its controls each
// frame, the host combines them and sends each frame back, and the other
// cabinets (the clients) queue those frames and play one a frame. Each client
// tells the host how many it has queued; while any has 2 or more, the host
// freezes a whole frame to let it catch up. Cabinets' clocks are never quite
// the same, and a frame's timing jitters, so a client often sits at the edge
// of that threshold and the host froze many times a race.
//
// Two things keep the race smooth:
// - Phase lock (each client): the client measures its queue every frame and
//   runs its machine's clock a little faster or slower (at most 0.2%, through
//   hw::g_link_clock_rate) to keep the queue near one frame, away from the
//   threshold, wherever the host's clock is.
// - Slack (the host): when the host checks the queues, it sees each one a
//   frame shorter, so it freezes only at 3, not 2. It costs a client at most a
//   frame of delay in the rare moment it needs it, instead of a freeze.
//
// With OPENGP_LOG_LINK_PACE set, each cabinet prints its queue, the host's
// freezes and the clock rate once a second during a linked race.
#pragma once

#include "gekko/types.h"

namespace link_pace {

// Where the game keeps the link's state.
struct Layout {
  gekko::u32 scene;          // the current scene's number (u32)
  gekko::u32 race_first, race_last;  // the scenes of a Versus race
  gekko::u32 link_active;    // a byte: the link is up
  gekko::u32 host_flag;      // a byte: 1 on the host
  gekko::u32 ring_read, ring_write;  // the client's queue of the host's frames (u32 counters)
  gekko::u32 ring_mask;      // the queue's size - 1
};

// After each frame's link exchange (send and receive), which took
// `exchange_ns` (a client ahead of the host waits in it for the host's frame).
void on_frame(gekko::u8* base, const Layout& layout, gekko::u64 exchange_ns);

// The host's queue check read a cabinet's frame entry at `entry`, whose byte
// at `queued_at` is that cabinet's queue: shown a frame shorter. `cabinet` is
// 0 to 3.
void on_queue_check(gekko::u8* base, gekko::u32 entry, gekko::u32 queued_at, int cabinet);

// The host froze a frame to let a client catch up.
void on_freeze();

}  // namespace link_pace
