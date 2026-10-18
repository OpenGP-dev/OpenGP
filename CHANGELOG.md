# Changelog

## 1.0.0 (not released yet)

1.0 follows a full audit of OpenGP: its code, build, installer packages, documents and licences.
This section lists everything that has changed since 0.4.0, including each fix from the audit as
it lands. 1.0 has the essential fixes (licences, saves, crashes, security, paths, the installer,
the documents); the larger improvements the audit found follow in 1.1.

### Even frame pacing

- **Even frame pacing** (Video tab, off by default; `[video] even_pacing`) shows each frame on the
  game's own 60 Hz tick, as a cabinet's display does, instead of as soon as it is drawn. The game
  takes more time over some frames than others, so they used to come 12 to 21 ms apart; a FreeSync
  or G-Sync monitor showed that unevenness as it came. Now they are presented at evenly spaced
  times, for about half a frame (8 ms) more delay. See [docs/playing.md](docs/playing.md#even-frame-pacing).

### Smoother

From a profile of a whole playthrough of GP 2:

- The busiest courses (Arena and Waluigi Stadium, the Mario courses, the flyovers before a race)
  drop far fewer frames. OpenGP transforms the game's vertices on the CPU, over 80,000 a frame
  there; each now takes about 40% less time, with exactly the same results.
- Shorter pauses when loading between scenes and at the start of a course: the game's file name
  lookup, memory copies and fills are done natively.
- The game image is held in memory, as on a cabinet: no more stutters, or the odd freeze of a
  second, while the game streams its music from the disc during a race.
- No more 4-40 ms renderer stalls in races where the game reads the picture back (Arena and
  Waluigi Stadium, the Mario courses), and shorter stalls when many textures load at once.
- Fixed a memory leak: every race kept a 64 MB texture at 8x (one of the game's shadow maps).
- The per-draw timers cost about half a millisecond a frame in busy races, even when nothing
  was measuring; they are now nearly free.
- Fewer dropped frames: the hardware's timed events (the display's retraces, the sound's DMA, the
  game's timers) now come on time on Windows. They used to come up to 2 ms late with the source
  build's compiler, and with the installer package's they kept a CPU core busy in the last
  millisecond before each one. In GP 2's attract mode at 8x, the game finished about 96% of its
  frames instead of about 87% on the same computer.
- Less GPU work at high internal resolutions, for the same picture: opaque surfaces let the GPU
  skip hidden pixels, the shader works only on the texture coordinates a surface has, and
  surfaces the game culls entirely are no longer sent to the GPU. A mesh's matrix change no longer
  rebuilds the draw state.

### Fixes

- Windows: OpenGP now works in folders whose names have characters outside the system's code page
  (an accent, Cyrillic, Chinese, Japanese...), such as a user folder named José or 日本. The
  installer used to fail to build the games there, the games could crash at start-up, and saves,
  settings, screenshots and images dropped on the installer could go unread or unwritten.
- The installer: an interrupted compiler download carries on where it stopped, an interrupted
  unpack is redone instead of leaving a broken compiler, and a failed download or unpack shows
  what went wrong. It remembers an image given to it by its full path, so the game finds it
  however it is started, and checks `--jobs`. Compiles running side by side no longer hold each
  other's output open.
- The games find their function names (for hang and crash reports) after the package folder is
  moved.
- Saves: Mario Cards and settings are written to the disk before they replace the old file, so a
  crash or power cut can no longer leave a card empty, and a save that another program (OneDrive,
  an antivirus, the search indexer) holds up for a moment is tried again. If a card, the game's
  records, the settings, the controls or a screenshot still cannot be saved, the game says so on
  screen instead of only on the console.
- One value OpenGP does not understand in `settings.toml` or `controls.toml` no longer throws away
  the whole file (and your link and card settings with it); a file it cannot read at all is kept as
  `settings.toml.bad` or `controls.toml.bad` instead of being overwritten. A key with no name can
  no longer be bound (it broke `controls.toml`), and `nan` no longer gets past the dead zone and
  force feedback limits. A missing `internal_resolution` no longer resets it.
- Crashes write `user/crash-report.txt` (and on Windows show where it is), also for aborts, fatal
  errors, uncaught exceptions and stack overflows, which used to vanish without a word. Hang
  reports say which version, game and time they are from, a second hang no longer overwrites the
  first, a hang before the first frame is reported too, and sleeping or hibernating the computer
  is no longer taken for a hang.
- If a game cannot start (no disc image, no Vulkan or Metal driver), it says why in a message too,
  not only on a console window that closes with it.
- A second copy of a game can no longer use the same user folder as one that is running: it says
  so and stops, instead of the two overwriting each other's saves. (GP and GP 2 can still run side
  by side from one package.)
- The menu's Resume button now saves the settings, as Esc does. Closing the console window saves
  and tells linked cabinets, as closing the game's window does. A wheel's forces and a gamepad's
  rumble stop when the game quits or restarts.
- A card name from `settings.toml` that cannot be a file name (`..`, `/`, `CON`...) holds no card.
- Widescreen: GP no longer leaves out objects in the side strips either (GP 2 was fixed earlier in
  this release).
- Wheels: with a steering wheel connected, steering comes from the wheel only, so separate pedals,
  a handbrake or a throttle can no longer turn it by their own first axis (an idle pedal used to
  hold the steering at full lock). An input a device does not have (an axis number past its last)
  is skipped instead of read as half pressed.
- Fixed a possible freeze in games that read the picture back (an EFB peek waited for the window's
  thread while holding what that thread needed).
- Crashes and freezes from bad game data or a struggling GPU: textures, palettes and copies above
  32 MB of memory are found (GP 2 has 48 MB), and out-of-range values in the game's graphics and
  sound commands, a damaged disc image's executable table, or the hang report's thread list are
  checked instead of read past their ends. A GPU that runs out of memory, or whose driver crashes,
  no longer crashes OpenGP or freezes a game waiting for a readback: the frame is skipped and, if
  it goes on, the player is told.
- Linked cabinets on different computers: each cabinet's messages now go out as soon as the game
  sends them. They used to wait for the other computer to acknowledge the one before, which it can
  delay by up to 200 ms, so they arrived in bursts and the other players' karts lagged behind
  their controls. Every computer in a linked race needs this version.
- Linked cabinets are harder to disrupt from the network. A cabinet only links with computers on
  the local network (or this one), never the internet; a cabinet's number stays with the cabinet
  that has it, so another computer can no longer take it over or send it away; a connection that
  says it is a cabinet must come from where that cabinet is; and connections that never introduce
  themselves, or that the game never takes, are closed instead of piling up.
- A cabinet running another version of OpenGP is no longer silently ignored: the Game tab names it
  ("OpenGP at 192.168.1.20 can't link with this one"). 1.0's link is not compatible with 0.4.0's.
- A linked game no longer freezes for up to 2 seconds when another cabinet refuses a connection,
  and a message the network could not take at once is sent whole later instead of cut short.
- When a game restarts (the test menu, a reboot, a lost link), the other cabinets now see its
  connections close at once, and the new process no longer inherits the old one's sockets, its
  webcam (Linux) or its memory.
- The rainbows of GP 2's Rainbow Coaster and Rainbow Downhill no longer nearly vanish at high
  internal resolutions: they fade in with distance as they should.
- Widescreen: GP 2 no longer leaves out objects in the side strips (scenery, item boxes, a blimp)
  that then popped in at the edges of the 4:3 picture.
- The webcam is on only while the game uses it: while your photo is taken, for a moment as the
  game starts, and on the test menu's camera page. It used to stay on, light and all, for as long
  as the game ran, taking about 5% of a CPU core for pictures nothing used.

### Licences

- The installer packages now carry the licences of the third-party code in them:
  `THIRD-PARTY-NOTICES.md` lists SDL 3, Dear ImGui, toml++, the boot ROM fonts, the XXH64 hash and
  the parts ported from Dolphin, and `licenses/` holds their licence files. README's Legal section
  names them and where OpenGP's source is.
- Every source file with code ported from Dolphin now says so itself, with Dolphin's copyright, not
  only its header.

### For contributors

- `CONTRIBUTING.md` (the no-game-data rule, licences, testing), `SECURITY.md`, and issue forms
  that ask for the version, the system and the hang report, and never for game files.
- CI checks every push for anything that looks like game data (`tools/check_game_data.py`: game
  file types, binaries, large files, a copy of a supported `main.dol`).
- CI and package workflows: actions pinned by commit and kept current by Dependabot, read-only
  tokens, time limits on jobs and tests, no duplicate runs, and a release job that only ever adds
  packages (with a `SHA256SUMS` file) to a draft release.
- `.gitattributes` keeps line endings LF on every platform, and `.gitignore` covers more build,
  editor and dump files.
- `tools/devshell.ps1` finds the Visual Studio installation that has Clang when a newer one
  without it is also installed, checks for Clang 16 or later, and leaves the PowerShell session's
  settings as they were.

## 0.4.0 (2026-10-16)

This release links cabinets for Versus races, on one computer or a local network, and shows what
you are playing on Discord. It comes with installer packages for Windows, Linux and macOS.

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
- GP 2 no longer freezes on the loading screen before a race with Bowser's **Flaming Tender**
  item. Its file is the last one in the disc image, and loading it reads a few bytes past the
  image's end, which OpenGP refused; the game then waited forever. Those bytes now read as zeros,
  as on a cabinet, where the image sits in a larger memory. Any read that still fails is printed.
- If the game's picture stops for 10 seconds while it is not paused, OpenGP writes a hang report,
  `user/hang-report.txt`: each of the game's threads with its call stack. See
  [docs/playing.md](docs/playing.md#if-the-game-freezes).

## 0.3.0 (2026-10-12)

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

## 0.2.0 (2026-10-06)

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

## 0.1.0 (2026-10-04)

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
