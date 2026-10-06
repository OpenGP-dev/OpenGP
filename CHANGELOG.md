# Changelog

## 0.2.0 (2026-09-26)

Installer packages for Windows, Linux and macOS: download, add your disc images, and the
installer builds the games on your machine. No game code or data is in the packages.

| Package | Compiler the installer uses |
|---------|-----------------------------|
| `OpenGP-0.2.0-windows-x64.zip` | llvm-mingw, downloaded once (about 190 MB) |
| `OpenGP-0.2.0-linux-x64.tar.gz` | the system's `g++` or `clang++` (GCC 12+ or Clang 16+) |
| `OpenGP-0.2.0-macos-arm64.tar.gz` | Apple's Clang (`xcode-select --install`); Apple Silicon, macOS 13+ |

See [docs/packaging.md](docs/packaging.md). Played on Windows and macOS; the Linux package is
built and tested in CI.

- **Installer package** ([docs/packaging.md](docs/packaging.md)): the runtime and recompiler
  prebuilt, and an installer (`OpenGP`) that builds each game on your machine from your own disc
  image. Either game can be installed on its own; running it again after an update rebuilds them.
- **macOS** (Apple Silicon): the renderer uses Metal (the shaders are converted to the Metal
  Shading Language at build time, so MoltenVK is not needed), the game's threads ask for
  performance cores, and there is an installer package for it.
- Images with a placeholder header (ID `RELSAB`, from some converted dumps) are recognised by their
  `main.dol` and work as they are.
- Fixed: with GCC 12 at `-O3`, paired-single results in recompiled code could miss their rounding
  to single precision.
- Packaged games keep `user/` next to them and remember the image they were built from
  (`user/images.toml`).
- Both games share one launcher `main()`; each game's ID, title and supported `main.dol` are in
  its `game.toml`.
- `--version`, and `OPENGP_LOG_INTERP=1` (code that falls back to the interpreter: none in either
  game any more, after the recompiler learned calls into the middle of functions).

Not yet: the NamCam2 camera, linked cabinets (next: milestone 10), and signed macOS packages
(open the package once with right-click, Open; see [docs/packaging.md](docs/packaging.md)).

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
