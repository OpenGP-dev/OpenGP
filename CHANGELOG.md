# Changelog

## Unreleased

### Linked cabinets (milestone 12)

- Up to four cabinets link for Versus races, as in an arcade: copies of OpenGP on one computer,
  or on computers on a local network. Give each its own **Cabinet number** in the menu's Game tab
  (`[link] cabinet`, or `--cabinet N`); on a local network, also turn on **Link over the local
  network** (`[link] lan`, or `--lan`). Start a game on one cabinet, and players on the others
  join it. Both games link, each with its own kind. See
  [docs/playing.md](docs/playing.md#linked-cabinets).
- The cabinets find each other by themselves, and connect only to cabinets playing the same game.
  A cabinet looks for the others as it starts: while others are running, it waits on its LINK
  screen until it has linked with them (up to 15 seconds). The Game tab shows who this cabinet is
  linked with.
- If a cabinet stops in the middle of a game, the others stop with "Link error occurred", as arcade
  cabinets do: OpenGP restarts them after a few seconds.

### Controls

- **Use the controls in the background too** (Controls tab) lets every OpenGP window read the
  controls, active or not, to drive several copies on one computer at once: gamepads everywhere,
  the keyboard on Windows.

### Discord Rich Presence (milestone 15)

- While Discord runs on the same computer, your profile shows what you are playing, as "Playing
  OpenGP": the game, and whether you are choosing a character or a course (or having your photo
  taken); in a race, the cup, round and course, your character, your place and the lap, with the
  race's time; then the place you finished, with the cup's trophy for a win. Both games, with their
  own names for everything and pictures of the characters and courses. It is on by default; turn it
  off in the menu's Game tab (**Show what you're playing**) or with `[discord] rich_presence` in
  `user/settings.toml`. See [docs/playing.md](docs/playing.md#discord).
- OpenGP talks to the Discord app through its local connection, without Discord's SDK, and sends
  nothing anywhere else. `OPENGP_LOG_DISCORD=1` prints what it shows.

### Fixes

- Widescreen: the rain in GP 2's DK Cup (rounds 3 and 4) covers the whole screen instead of
  stopping at the edges of the 4:3 picture, and so does the fade at the end of the attract mode's
  demo race.
- If the game's picture stops for 10 seconds while it is not paused, OpenGP writes a hang report,
  `user/hang-report.txt`: each of the game's threads with its call stack. See
  [docs/playing.md](docs/playing.md#if-the-game-freezes).

## 0.3.0 (2026-10-01)

This release adds the cabinet's camera, from a webcam, and a pause. It is source only, with no
installer packages: build it as [docs/building.md](docs/building.md) explains. It has been
built and tested on Windows; the Linux and macOS builds have not been tested since milestone 10.

### The camera (milestone 11)

- The cabinet's camera, the NamCam2, can use a webcam: both games take the player's photo when a
  game starts and show it on their character in the race. It is off by default; turn it on in
  the menu's Game tab (**Photos from**), in `user/settings.toml` (`[camera] source`) or with
  `--camera webcam`. A test pattern (`--camera test`) stands in without a webcam. See
  [docs/playing.md](docs/playing.md#the-camera).
- The media board's network works: the games' sockets run on host sockets, or on services OpenGP
  answers itself. The camera is one, an HTTP server that sends a JPEG, so it needs no network
  settings and opens no ports. Connections to other cabinets are refused until linked cabinets
  (milestone 12); `OPENGP_LOG_NETWORK=1` shows what the games try.
- With the camera on, the start-up device check finds it in about a second. On a cabinet it waits
  40 seconds for the camera to power up.

### The GP 2 decomp's findings (milestone 10)

- Both games' symbol maps have the names of functions and globals from
  [mkgp2-decomp](https://github.com/naari3/mkgp2-decomp): 3,427 functions and 488 objects in
  GP 2, about 1,000 functions in GP. Backtraces and profiles show them.
- `--game-log` prints the games' own debug messages, converted from Shift JIS.

### Pause (milestone 14)

- **P** (or the Pause key) pauses the game, and the menu (Esc) pauses it while it is open. The
  whole machine stops: the game, its timers, its sound and the wheel's forces, and it carries on
  as if no time had passed. Optionally, the game also pauses while its window is unfocused.
  Pausing is only for playing solo: a game linked to other cabinets keeps running. See
  [docs/playing.md](docs/playing.md#pausing).

### Smaller changes

- GP 2's attract mode has sound every time round. The game plays its first time round without
  sound on purpose (it starts quiet and turns the sound on for the rounds after), which made it
  seem broken.
- Clearer wording in the menu's Game tab.

Still missing before 1.0 are linked cabinets (milestone 12).

## 0.2.0 (2026-09-26)

This release adds installer packages for Windows, Linux and macOS. You download a package and put
your disc images in it, and its installer builds the games on your computer. The packages contain
no game code or data.

| Package | Compiler the installer uses |
|---------|-----------------------------|
| `OpenGP-0.2.0-windows-x64.zip` | llvm-mingw, downloaded once, about 190 MB |
| `OpenGP-0.2.0-linux-x64.tar.gz` | the system's `g++` or `clang++`, GCC 12 or Clang 16 or later |
| `OpenGP-0.2.0-macos-arm64.tar.gz` | Apple's Clang from `xcode-select --install`, on Apple Silicon with macOS 13 or later |

[docs/packaging.md](docs/packaging.md) explains how the packages work. The games have been played
from the Windows and macOS packages. CI builds and tests the Linux package, but nobody has played
from it yet.

- The installer package holds the runtime and the recompiler, already built, and an installer
  named `OpenGP`. The installer builds each game from your own disc image. You can install either
  game on its own, and running the installer again after an update rebuilds them.
- OpenGP runs on macOS with Apple Silicon. The renderer uses Metal, so MoltenVK is not needed. The
  build converts the shaders to the Metal Shading Language, and the game's threads ask macOS for
  its performance cores.
- Some converted dumps have a placeholder header with the ID `RELSAB`. OpenGP now recognises
  these images by their `main.dol`, so they work without being patched.
- GCC 12 at `-O3` could drop the rounding to single precision from paired-single results in
  recompiled code. `fp::keep_single` stops it.
- A packaged game keeps `user/` next to its executable and remembers the image it was built from
  in `user/images.toml`.
- Both games share one launcher `main()`. Each game's ID, title and supported `main.dol` are in its
  `game.toml`.
- `--version` prints the version. `OPENGP_LOG_INTERP=1` lists code that falls back to the
  interpreter. Neither game has any now, because the recompiler handles calls into the middle of
  functions.

Still missing are the NamCam2 camera, linked cabinets, which are next in milestone 10, and signed
macOS packages. Until the packages are signed, open a macOS package once with right-click and
Open, as [docs/packaging.md](docs/packaging.md) describes.

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
