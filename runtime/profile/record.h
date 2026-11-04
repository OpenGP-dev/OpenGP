// SPDX-License-Identifier: GPL-3.0-or-later
//
// A profile's per-frame record (frames.bin): one per displayed frame, fixed
// size, little-endian: the time it was presented (a u64 of microseconds
// since the profile started), then the u32 fields below in order. A
// profile's session.json describes the layout (struct format and field
// names), so tools follow the list as it grows. Times are in microseconds.
#pragma once

#include <cstddef>

#include "gekko/types.h"

namespace profile {

// Flag bits of FrameRecord::flags.
enum FrameFlag : gekko::u32 {
  kPaused = 1u << 0,
  kMenuOpen = 1u << 1,
  kBackground = 1u << 2,  // the window was not the active one
  kLinked = 1u << 3,
  kHitch = 1u << 4,
  kMark = 1u << 5,        // the player marked the moment (the mark control)
  kCapture = 1u << 6,     // the frame was captured (the capture control)
  kThumbnail = 1u << 7,   // a thumbnail was asked for (it is of this frame or the next)
  kVsync = 1u << 8,
  kFullscreen = 1u << 9,
  kEvenPacing = 1u << 10,
};

#define OPENGP_FRAME_FIELDS(X)                                                                                  \
  X(frame, "displayed frame number, counted from power-on (across the game's restarts)")                       \
  X(retrace, "VI retraces so far (59.94 a second)")                                                             \
  X(flags, "1 paused, 2 menu open, 4 window in the background, 8 linked, 16 hitch, 32 mark, "                   \
           "64 capture, 128 thumbnail, 256 vsync, 512 fullscreen, 1024 even pacing")                            \
  X(scene, "the game's scene number (see the scene events)")                                                    \
  X(race, "in a race: position | lap << 8 | laps << 16 | started << 24 | finished << 25")                       \
  X(guest_us, "game: time since its previous frame (its frame time)")                                           \
  X(idle_us, "game: ... of which it was idle, waiting for an interrupt (its spare time)")                       \
  X(wait_us, "game: ... of which it waited for the renderer to take frames")                                    \
  X(readback_wait_us, "game: ... of which it waited for EFB readbacks")                                         \
  X(gx_us, "game: ... of which in the GX front end's draws (state, textures and transforms included)")          \
  X(gx_state_us, "GX front end: building draw state")                                                           \
  X(gx_texture_us, "GX front end: looking up textures (decoding included)")                                     \
  X(gx_decode_us, "GX front end: decoding textures")                                                            \
  X(gx_transform_us, "GX front end: loading and transforming vertices")                                         \
  X(calls, "primitives the game drew")                                                                          \
  X(culled, "... of which nothing was left after culling")                                                      \
  X(draws, "host draw commands (primitives merged)")                                                            \
  X(vertices, "vertices transformed")                                                                           \
  X(triangles, "triangles drawn")                                                                               \
  X(states, "distinct draw states")                                                                             \
  X(binds, "texture lookups")                                                                                   \
  X(decodes, "textures decoded (texture cache misses)")                                                         \
  X(decoded_kb, "KiB of RGBA8 texels decoded")                                                                  \
  X(hashed_kb, "KiB of guest memory hashed by the texture cache")                                               \
  X(copy_binds, "texture lookups that used an EFB copy")                                                        \
  X(copies, "EFB copies to textures")                                                                           \
  X(clears, "EFB clears")                                                                                       \
  X(partials, "partial frames (rendered early for EFB readbacks)")                                              \
  X(readbacks, "EFB readbacks asked for")                                                                       \
  X(cached_textures, "textures in the GX texture cache")                                                        \
  X(depth_equal, "draws with an 'equal' depth test (sensitive to depth precision)")                             \
  X(widescreen, "draws the widescreen heuristics changed: widened 2D | extended edges << 10 | "                 \
                "widened overlays << 20")                                                                        \
  X(risks, "likely glitches noted (see the risk events)")                                                       \
  X(render_us, "renderer: rendering the frame (and the partial frames before it)")                              \
  X(upload_us, "renderer: ... copying vertices, indices and textures for the GPU")                              \
  X(upload_kb, "renderer: KiB copied for the GPU")                                                              \
  X(acquire_us, "renderer: ... waiting for the window's next image (vsync, or a busy GPU)")                     \
  X(submit_us, "renderer: ... submitting the commands")                                                         \
  X(fence_us, "renderer: ... waiting for the GPU (readbacks, screenshots, GPU timing)")                         \
  X(gpu_us, "the GPU's time for the frame (only with OPENGP_PROFILE_GPU_SYNC=1)")                               \
  X(pipeline_us, "renderer: ... creating pipelines (shader compiles)")                                          \
  X(pipelines, "pipelines created")                                                                             \
  X(textures, "GPU textures created")                                                                           \
  X(texture_kb, "KiB of GPU textures created")                                                                  \
  X(gpu_draws, "draw calls recorded for the GPU")                                                               \
  X(passes, "render passes")                                                                                    \
  X(live_textures, "GPU textures alive")                                                                        \
  X(queue_us, "main thread: waiting for the game's frame before rendering it")                                  \
  X(queue_depth, "frames waiting to be rendered after this one")                                                \
  X(latency_us, "from the game finishing the frame to the renderer finishing it")                               \
  X(present_us, "time since the previous frame was presented")                                                  \
  X(audio_queue_ms10, "sound output queue, in tenths of a millisecond")                                         \
  X(audio_underruns, "times the sound ran dry since the previous frame")                                        \
  X(exceptions, "interrupts delivered to the game since the previous frame")                                    \
  X(drops, "VI retraces that went by without a new frame before this one (it was late)")                        \
  X(guest_busy_us, "game: ... of which it was at work (its frame time less idle, waiting and readbacks)")        \
  X(pipeline_binds, "renderer: pipelines bound for draws")                                                      \
  X(live_texture_kb, "KiB of GPU textures alive after the frame")                                                \
  X(live_pipelines, "pipelines alive after the frame")                                                          \
  X(pace_us, "main thread: holding the frame until the game's retrace (even frame pacing)")

struct FrameRecord {
  gekko::u64 t_us = 0;  // when it was presented, since the profile started
#define OPENGP_FRAME_FIELD(name, doc) gekko::u32 name = 0;
  OPENGP_FRAME_FIELDS(OPENGP_FRAME_FIELD)
#undef OPENGP_FRAME_FIELD
};

#define OPENGP_FRAME_FIELD(name, doc) +1
inline constexpr std::size_t kFrameFieldCount = 0 OPENGP_FRAME_FIELDS(OPENGP_FRAME_FIELD);
#undef OPENGP_FRAME_FIELD
// The fields follow t_us without gaps (written as they are).
static_assert(offsetof(FrameRecord, frame) == sizeof(gekko::u64));

}  // namespace profile
