# OpenGP

OpenGP is a **native PC port of Mario Kart Arcade GP and Mario Kart Arcade GP 2**
(Namco, 2005 and 2007, Triforce arcade hardware). It is built by **static recompilation**, the
same approach as [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).

The game's PowerPC (Gekko) machine code is translated into C++ ahead of time. That C++ is then
compiled together with a runtime that stands in for the Triforce hardware (GameCube GPU, audio
DSP, AM-Baseboard, AM-Mediaboard), so the game runs as a normal native program, with modern
features such as higher resolutions, widescreen and remappable controls.

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
| App | `runtime/app/` | Command line, settings, window, the in-game menu (Dear ImGui) |
| Games | `games/` | Per-game manifest, symbol maps, recompiler config and patches (`mkgp1`, `mkgp2`); what both share in `common/` (their `main()`, SDK replacements, the arcade library's free play and test-menu restart) |
| Installer | `tools/installer/` | Builds the games on the player's machine from the installer package: the library the package's launcher uses, and `opengp-install` (see [docs/packaging.md](docs/packaging.md)) |
| Scripts | `tools/*.py`, `tools/devshell.ps1` | Game setup (extract and verify `main.dol`), naming functions (SDK signatures, the GP 2 decomp, matching functions between the games), Windows build shell |
| Tests | `tests/` | Decoder tests, known-answer semantics tests, and differential tests (interpreter vs recompiled code) |

Details are in [docs/architecture.md](docs/architecture.md). Notes on the arcade hardware are in
[docs/triforce.md](docs/triforce.md).

## Made with AI

OpenGP is developed with AI. The maintainer decides what gets built and plays the results; most
of the code and documentation is written by Claude (Anthropic's model) through
[Claude Code](https://claude.com/claude-code). Commits made that way end with a
`Co-Authored-By: Claude` line, so the history shows which ones.

The code is held to the same checks as any other:
- The test suite (decoder tests, known-answer semantics tests, and differential tests that run the
  same guest code through the interpreter and the recompiled code) runs in CI on every push, on
  Linux, Windows and macOS.
- The hardware follows Dolphin's behaviour, and code ported from Dolphin is marked as such.
- What OpenGP learned from other projects is credited below and in the docs. Nothing is copied
  from projects that have no license, only facts such as addresses.

**TLDR, i check through most commits before committing. i use Claude to commit. so some human commits made by me are also co-authored by Claude.** 

## Playing

Download the launcher package for Windows, Linux or macOS (Apple Silicon) from the releases,
unpack it and open `OpenGP`, the launcher. Add your disc image of each game: OpenGP builds the games
on your machine from your images, then plays them (see [docs/packaging.md](docs/packaging.md) and
[docs/playing.md](docs/playing.md)). On the command line, `opengp-install` builds them the same way.

## Building

You need CMake 3.25+, Ninja, Python 3, and GCC 12+ or Clang 16+. On Windows, use clang-cl: the
recompiled code relies on weak symbols, which MSVC does not support. Full instructions, including
Windows setup, are in [docs/building.md](docs/building.md).

```sh
python tools/setup.py            # extracts main.dol from your images in ISO/ (see below)
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
- `python tools/setup.py` extracts each game's `main.dol` into `orig/` (also gitignored), and the
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
fonts in `thirdparty/gc-fonts` (Dolphin's free replacements, based on Droid Sans, Apache-2.0), Qt 6
in the packages' launcher (LGPL-3.0) and others, all listed with their licences in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Releases come with installer packages for Windows, Linux and macOS. 0.1.0 to 1.0.0 are internal
releases: they are tags in this repository, with their notes in [CHANGELOG.md](CHANGELOG.md).

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
