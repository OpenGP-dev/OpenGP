# Profiling

OpenGP can record everything about how the game runs while you play, so that
slowdowns, stutters and graphical glitches can be found and fixed afterwards
without having to reproduce them. A recording is a *profile*: a folder of
files made to be read by a tool ([`tools/profiles.py`](../tools/profiles.py)),
by a developer or by an AI assistant.

## Recording while you play

| Key | What it does |
|-----|--------------|
| **F10** | Starts recording a profile, and stops it (the Profiler tab in the Esc menu does the same) |
| **F8** | Marks the moment: press it right after a stutter or when something looks wrong, so it can be found in the profile |
| **F9** | Captures the next frame: every texture, every draw and what drew it (see [Frame captures](#frame-captures)). Press it while a glitch is on screen |

Or start the game with `--profile` to record from the start. Recording stops
when the game quits; a game that restarts itself (the test menu) goes on
recording into the same profile.

Profiles are saved in `user/profiles/<date>_<time>/`. To send one, zip its
folder. A profile holds small pictures of the screen and a frame capture holds
the game's textures from your disc: share them with the people fixing the
game, but don't post them publicly.

What it costs: each call stack sample stops the thread it samples for about
25 microseconds, so at the default 500 samples a second the game loses about
1% of its time (`--profile-rate 1000` for finer detail, `--profile-rate 0` for
none). The rest of the recording happens on other threads. A scene where the
game is close to missing frames may miss a few more while recording.

## What a profile holds

| File | What's in it |
|------|--------------|
| `session.json` | The game, OpenGP's version and commit, the computer (OS, CPU, memory), and the per-frame record's layout |
| `frames.bin` | One record per frame shown (`profile/record.h`): the game's frame time and how much of it was spent idle (spare time), in the GX front end (draw state, texture lookups and decoding, vertex transforms), waiting for the renderer or for EFB readbacks; the renderer's time (uploads, waiting for the display or GPU, shader compiles); draws, triangles, textures decoded, EFB copies; the scene, race position and lap; pause and menu state. `profiles.py fields` lists them all |
| `events.jsonl` | One JSON object per line, in time order. `"e"` says what: `start`/`resume`/`end`, `scene` (the game's scene, mode, course, character), `race_start`, `lap`, `race_end`, `hitch` (a frame much longer than usual, with what took the time and the game's busiest functions then), `risk` (a likely glitch, below), `pipelines` (shaders compiled, and how long it took), `settings`, `efb` (the internal resolution or widescreen width changed), `paused`/`unpaused`, `menu_opened`/`menu_closed`, `background`/`foreground`, `mark`, `capture`, `screenshot`, `hang`, `link_lost`. `"t"` is seconds since the start, `"f"` the frame |
| `system.jsonl` | Once a second: CPU use of each thread by name (`main` renders, `guest N` run the game, `timer` runs the hardware's timed events), of OpenGP and of the whole computer (other programs competing), memory, page faults |
| `console.log` | Everything OpenGP printed |
| `cpu/run<N>/` | Call stack samples of every busy thread (Windows): `samples.bin`, `stacks.bin`, `symbols.tsv`, `threads.tsv` (see `profile/monitor.h`). Recompiled game functions show under the game's names |
| `thumbs/f<frame>.jpg` | A small picture of the screen every 2 seconds (`--profile-thumbs N` frames), and of every scene change, hitch, likely glitch and mark |
| `captures/` | Frame captures made while recording |
| `summary.json` | Frame time percentiles, hitches by cause, every scene's frame times, the likely glitches with counts. Written when recording stops |

### Hitches

A frame is a hitch when it takes much longer than the frames before it (half
as long again, and at least 8 ms more). Its event says where the time went:
`game code` (the game's own work), `texture decoding`, `GX front end`,
`shader compiles`, `waiting for the display or GPU`, `uploads to the GPU`,
`renderer`, `EFB readbacks`, or `unexplained` (the time went nowhere OpenGP
measures: another program, the OS). With call stack samples it also lists the
functions the game was in (`self`) and the ones on its stack (`inclusive`).

### Likely glitches (risks)

The GX front end notes things it can't draw as the hardware would: a texture
outside memory or in an invalid format (drawn white), a preloaded RGBA8
texture, a texture read where an EFB copy of another size went (the copy isn't
used, and memory the copy never wrote is read instead), an EFB copy read in
another format, CPU reads of the EFB before a readback was ready, CPU writes
to the EFB, z-freeze. Each distinct one is an event the first time, with a
thumbnail; `summary.json` counts them.

## Frame captures

**F9** (or `--capture-frame FRAME`, repeatable, or the Profiler tab) saves one
frame with everything needed to see why it looks wrong, in the profile being
recorded (`captures/<n>_f<frame>/`) or else in `user/captures/`:

| File | What's in it |
|------|--------------|
| `capture.txt` | Every command in order. Each draw: primitive, vertex count, triangles, where it is on screen (game pixels) and its depth range, blending and depth test, culling, the TEV stages as formulas, its textures (address, format, size, sampling, and the file), vertex format and lighting, any widescreen adjustment, and the game function that drew it with its callers. Each EFB copy (with its result's file) and clear. At the top: draws and triangles by the function that drew them, and by the code that owns them outside the game's engine |
| `capture.json` | The same, for tools |
| `screen.png` | What was shown, at the internal resolution |
| `efb_end.png`, `depth_end.png` | The EFB just before it was shown, and its depth (near is white); `depth_end_raw.png` holds the exact 24-bit depths (red, green and blue bytes, red highest) |
| `efb_start.png`, `depth_start.png` | The same as the frame found them |
| `textures/` | Every texture the frame used, decoded (`t<handle>_<address>_<format>_<w>x<h>.png`) |
| `copies/` | Every EFB copy's result |
| `earlier/` | EFB copies made in earlier frames that this one samples |

A captured frame takes a moment to save: the game pauses for a fraction of a
second.

## Reading profiles: tools/profiles.py

Every command prints compact text. With no profile named it reads the newest
in `user/profiles/`.

```
python tools/profiles.py list                      # the profiles
python tools/profiles.py summary                   # everything at a glance
python tools/profiles.py scenes                    # frame times, draws and hitches by scene
python tools/profiles.py hitches --top 20          # the longest frames: causes, hot functions
python tools/profiles.py cpu --slow 25 --stacks 5  # where the game's time goes in frames over 25 ms
python tools/profiles.py cpu --from 3000 --to 3600 # ... in a stretch of frames
python tools/profiles.py frame 3412                # one frame: every field, nearby events, thumbnail
python tools/profiles.py frames --where "guest_us > 20000" --fields frame,scene,guest_us,idle_us,gx_us
python tools/profiles.py events --type scene,mark,risk
python tools/profiles.py risks                     # likely glitches
python tools/profiles.py system                    # CPU and memory over time
python tools/profiles.py thumbs --sheet sheet.png --from 3000 --to 4000   # a contact sheet (Pillow)
python tools/profiles.py compare OTHER             # before and after a change
python tools/profiles.py capture                   # the newest frame capture
python tools/profiles.py capture --png 850,500     # the draws covering a pixel of screen.png
python tools/profiles.py capture --draw 2400       # one command in full
python tools/profiles.py capture --grep efb_copy   # commands mentioning something
```

The frame time to look at first is `guest_us` (the game's frame time). A game
that keeps up shows 16.7 ms frames with idle time to spare (`idle_us`); a frame
that misses its retrace takes 33 ms (`drops` counts the retraces that went by
without a new frame).

For timing the GPU itself, `OPENGP_PROFILE_GPU_SYNC=1` makes the renderer wait
for each frame to finish on the GPU and records how long it took (`gpu_us`).
It costs the overlap between the CPU and the GPU, so frame rates drop.
