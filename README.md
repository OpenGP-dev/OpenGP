# OpenGP

OpenGP is a **native PC port of Mario Kart Arcade GP and Mario Kart Arcade GP 2**
(Namco, 2005 and 2007, Triforce arcade hardware). It is built by **static recompilation**, the
same approach as [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).

The game's PowerPC (Gekko) machine code is translated into C++ ahead of time. That C++ is then
compiled together with a runtime that stands in for the Triforce hardware (GameCube GPU, audio
DSP, AM-Baseboard, AM-Mediaboard), so the game runs as a normal native program, with modern
features such as higher resolutions, widescreen and remappable controls.

> **Status: 1.0, feature-complete.** Milestones 1 to 12, 14 and 15 are done, and OpenGP is
> feature-locked: only bug fixes and design tweaks from here. Mario Kart Arcade GP 2 (7,612
> functions) and Mario Kart Arcade GP (7,504 functions) recompile, boot from their disc images
> through the emulated Triforce hardware (media board, baseboard, JVS I/O, card reader) and play in
> a window or fullscreen (Vulkan, or Metal on macOS), in 4:3 or widescreen and at up to 8 times
> their resolution: insert a coin (or turn on free play), make a Mario Card, pick a character and
> cup, and race at 60 fps with the keyboard, a gamepad or a steering wheel with force feedback, with
> the games' music and sound effects (their DSP audio microcode is emulated). A webcam can stand in
> for the cabinet's camera, up to four cabinets link for Versus races (on one computer or a local
> network), you can pause when you play solo, and Discord shows what you are playing. A menu (Esc) has the settings, the controls and the Mario Cards; the games' own test
> menus work too. See [docs/playing.md](docs/playing.md) and the [milestones](#milestones).

The latest release is **1.0.1**, with the fixes from a full audit of the code, build, packages,
documents and licences, and installer packages for Windows, Linux and macOS (see
[CHANGELOG.md](CHANGELOG.md)).

## How it works

```
your GGPE02.iso ─extract─> main.dol ─decomp-toolkit─> symbols.txt
                               │                          │
                               └───> gekkorecomp <────────┴── games/mkgp2/recomp.toml
                                         │
                            build/generated/*.cpp   (never committed)
                                         │
                   + runtime/ (C++20) ───┴──> OpenGP executable
```

| Part | Path | What it does |
|------|------|--------------|
| Gekko ISA | `common/gekko/` | Decoder, disassembler and encoder for every Gekko instruction, including paired singles |
| Recompiler | `tools/gekkorecomp/` | Turns a DOL plus a symbol list into C++, one C++ function per guest function |
| CPU runtime | `runtime/ppc/` | Register context, guest memory, instruction semantics, interpreter fallback, indirect-call dispatch |
| Machine | `runtime/hw/`, `runtime/os/`, `runtime/triforce/`, `runtime/gx/` | Hardware models (interrupts, timers, VI, DSP, AI, SI, EXI, DI), boot and guest threads, the Triforce boards, and the GPU (command processor, vertex loading and transform, textures, EFB copies) |
| Renderer | `runtime/video/` | SDL3 GPU renderer: the TEV ubershader, EFB, presenting |
| Input | `runtime/input/` | Keyboard, gamepads and wheels (SDL3) mapped onto the cabinet's controls, force feedback, scripted input |
| Launcher | `runtime/app/` | Command line, settings, window, the in-game menu (Dear ImGui) |
| Games | `games/` | Per-game symbol maps, recompiler config, patches and launcher (`mkgp1`, `mkgp2`); what both share in `common/` (SDK replacements, the arcade library's free play and test-menu restart) |
| Installer | `tools/installer/` | Builds the games on the player's machine from the installer package (see [docs/packaging.md](docs/packaging.md)) |
| Scripts | `tools/*.py`, `tools/devshell.ps1` | Game setup (extract and verify `main.dol`), naming functions (SDK signatures, the GP 2 decomp, matching functions between the games), Windows build shell |
| Tests | `tests/` | Decoder, known-answer semantics and differential tests (interpreter vs recompiled code), and tests of the hardware, renderer front end, settings, input, link, paths and crash reports |

Details are in [docs/architecture.md](docs/architecture.md). Notes on the arcade hardware are in
[docs/triforce.md](docs/triforce.md).

## Made with AI

OpenGP is developed with AI. The maintainer decides what gets built and plays the results; most
of the code and documentation is written by Claude (Anthropic's model) through
[Claude Code](https://claude.com/claude-code). Commits made that way end with a
`Co-Authored-By: Claude` line, so the history shows which ones.

The code is held to the same checks as any other:
- The test suite (among others, differential tests that run the same guest code through the
  interpreter and the recompiled code) runs in CI on every push that changes the code, on Linux,
  Windows and macOS.
- The hardware follows Dolphin's behaviour, and code ported from Dolphin is marked as such.
- What OpenGP learned from other projects is credited below and in the docs. Nothing is copied
  from projects that have no license, only facts such as addresses.

## Playing

Download the installer package for Windows, Linux or macOS (Apple Silicon) from the releases, unpack it, put your disc
images in its `ISO` folder and run `OpenGP`: it builds the games on your machine from your images
(see [docs/packaging.md](docs/packaging.md)), then play with `opengp-mkgp2` or `opengp-mkgp1`
([docs/playing.md](docs/playing.md)).

## Building

You need CMake 3.25+, Ninja, Python 3.9+, and GCC 12+ or Clang 16+. On Windows, use clang-cl: the
recompiled code relies on weak symbols, which MSVC does not support. Full instructions, including
Windows setup, are in [docs/building.md](docs/building.md).

```sh
python3 tools/setup.py           # extracts main.dol from your images in ISO/ (Windows: python)
cmake --preset default           # Windows: . .\tools\devshell.ps1, then --preset clang-cl
cmake --build --preset default
ctest --preset default
```

Without the game you still get the recompiler, the CPU runtime and the test suite. The tests need
no game data: they generate their own PowerPC test programs.

## Legal

This repository contains **no game code, data or assets**, and none may ever be added. That
includes the recompiler's output, because it is derived from the game.

To play, you will need your own copy of the game:
- Put your images in `ISO/` (or `private/`). Both folders are gitignored.
- `python3 tools/setup.py` extracts each game's `main.dol` into `orig/` (also gitignored), and the
  build reads it from there.
- [docs/game-files.md](docs/game-files.md) explains how to prepare them.

Mario Kart and Mario Card are trademarks of Nintendo and Namco; Pac-Man and Tamagotchi are
trademarks of Bandai Namco; other names belong to their owners. This project is not affiliated
with or endorsed by Nintendo, Bandai Namco or Sega.

OpenGP is Copyright (C) 2026 the OpenGP contributors and is licensed under the **GPL-3.0-or-later**
(see [LICENSE](LICENSE)). Its source code is at <https://github.com/OpenGP-dev/OpenGP>. Parts of the
runtime and tools are ported from the [Dolphin emulator](https://github.com/dolphin-emu/dolphin)
(GPL-2.0-or-later) and are marked as such in the source. The third-party code OpenGP and its
packages include keeps its own licence: SDL 3 (zlib), Dear ImGui and toml++ (MIT), the boot ROM
fonts in `thirdparty/gc-fonts` (Dolphin's free replacements, based on Droid Sans, Apache-2.0) and
others, all listed with their licences in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Milestones

OpenGP is feature-locked at 1.0, with only bug fixes and design tweaks from here on.

| # | Milestone | Needs the game |
|---|-----------|----------------|
| M1 | Scaffolding, recompiler, CPU runtime, test suite, CI (**done**) | no |
| M2 | MKAGP2 (GGPE02) analysis: symbol map, SDK functions identified, whole game recompiles and builds (**done**) | yes |
| M3 | Boot to the main loop: guest threads, hardware models, media board, baseboard and JVS, card reader, GPU FIFO (**done**) | yes |
| M4 | Graphics: vertex loader, textures, TEV ubershader, first frames, attract mode; hardware-check patches as needed (**done**) | yes |
| M5 | Input and gameplay: JVS mapping, free play, full race, wheel force feedback (**done**) | yes |
| M6 | Audio: the DSP's AX microcode (driven by MusyX) at a high level, sound output (**done**) | yes |
| M7 | Card saves, settings menu, resolution scaling, widescreen, fullscreen; the game's test menu (**done**) | yes |
| M8 | Mario Kart Arcade GP (GGPE01): symbol map, the same runtime, free play, test menu (**done**) | yes |
| M9 | Installer packages for Windows, Linux and macOS (Apple Silicon, Metal renderer): the prebuilt runtime and recompiler; the installer recompiles and builds the games from the player's own images, so no game code is ever distributed (**done**, 0.2.0) | yes |
| M10 | What the GP 2 decomp ([mkgp2-decomp](https://github.com/naari3/mkgp2-decomp)) knows: its function names in both games' symbol maps, and the games' own debug messages (`--game-log`), for working on M11 and M12 (**done**, 0.3.0) | yes |
| M11 | The NamCam2 camera from a webcam: player photos. The camera is an HTTP server on the media board's network, so this starts the network (the socket API on host sockets) (**done**, 0.3.0) | yes |
| M12 | Linked cabinets: the rest of the media board's network, several instances racing each other on one PC or a LAN (**done** ahead of 1.0, 0.4.0) | yes |
| M14 | Pause: freeze the game (its threads, sound and timers) from the menu or a button; the arcade games have no pause of their own. Only when playing solo: a game linked to other cabinets (M12) keeps running (**done** ahead of 1.0, 0.3.0) | yes |
| M15 | Discord Rich Presence: the character, course and race position, read from the game's state (the decomp's names) and sent over Discord's local connection, without Discord's SDK (**done** ahead of 1.0, 0.4.0) | yes |
| 1.0 | Release: the installer package with linked cabinets and the camera (**done**, 1.0.0) | yes |

Releases come with installer packages for Windows, Linux and macOS. 0.1.0 to 0.4.0 were internal
releases: they are tags in this repository, with their notes in [CHANGELOG.md](CHANGELOG.md).
1.0.0 is the first release.

## Acknowledgements

- **[Dolphin](https://github.com/dolphin-emu/dolphin)**: the reference for Gekko, GameCube and
  Triforce behaviour, and the source of the runtime's ported parts.
- **[mkgp2-decomp](https://github.com/naari3/mkgp2-decomp)** by naari3: the names of GP 2's
  functions and globals (and through them GP's), and notes on the games' network code.
- **[mkgp2-patch](https://github.com/naari3/mkgp2-patch)** by naari3: the Ghidra analysis of GP 2
  that the decomp's names grew out of.
- **[Skip-checks patch](https://github.com/coremaven/Mario-Kart-Arcade-GP-2---Skip-Checks-Patch)**
  by coremaven: where GP 2's hardware checks are, which OpenGP's camera and cabinet-link patches
  are based on.
- **[decomp-toolkit](https://github.com/encounter/decomp-toolkit)**: GameCube binary analysis.
- **N64Recomp / Zelda64Recomp** and **XenonRecomp / UnleashedRecomp**: the overall design.
