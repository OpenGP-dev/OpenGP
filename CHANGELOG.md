# Changelog

## 0.1.0 (2026-09-26)

The first release: both Mario Kart Arcade GP games are playable from start to finish.

This is a **source release**. OpenGP contains no game code or data, and neither does this release:
you build it yourself from your own copy of the games (see [Building](README.md#building) and
[docs/game-files.md](docs/game-files.md)). Each build runs only with the disc image it was made for.

### Games

| Game | ID | Launcher |
|------|----|----------|
| Mario Kart Arcade GP 2 (USA) | `GGPE02` | `opengp-mkgp2` |
| Mario Kart Arcade GP (USA) | `GGPE01` | `opengp-mkgp1` |

Both are fully recompiled (7,612 and 7,504 functions), with nothing left to the interpreter in play.

### What works

- **Playing**: start-up device check, attract mode, coins or free play (`--free-play`, or the
  menu), Mario Card creation and saving, character and cup selection, Grand Prix races at 60 fps,
  results and continue.
- **Controls**: keyboard, gamepads and steering wheels/pedals (SDL3), rebindable in the menu or
  `user/controls.toml`; force feedback on wheels (spring, friction, road feel) and rumble on
  gamepads.
- **Graphics**: Vulkan, internal resolution up to 8x, widescreen (16:9 up to 32:9) with more of the
  track on the sides and the 2D kept in shape, fullscreen.
- **Sound**: the game's music and effects (its DSP audio microcode emulated at a high level).
- **Mario Cards**: one file per card in `user/cards/<game>/`, chosen in the menu.
- **Operator settings**: the game's own test menu (F2, or `--test-menu`), and the I/O test.
- **Menu** (Esc): Mario Card, free play, video, sound and controls settings, saved to
  `user/settings.toml`.

### Not yet

- The NamCam2 camera is disabled (no player photos).
- Cabinets cannot be linked (no multiplayer between machines).
- Played on Windows only. Linux builds and passes the tests in CI but has not been played; macOS
  is untested (the renderer is Vulkan only, no Metal yet).
- Only the USA releases are supported.

### Known issues

- Races need most of a CPU core: a PC busy with something else (another game) can drop below
  60 fps.
