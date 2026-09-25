# OpenGP

OpenGP is a work-in-progress **native PC port of Mario Kart Arcade GP and Mario Kart Arcade GP 2**
(Namco, 2005 and 2007, Triforce arcade hardware). It is built by **static recompilation**, the
same approach as [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).

The game's PowerPC (Gekko) machine code is translated into C++ ahead of time. That C++ is then
compiled together with a runtime that stands in for the Triforce hardware (GameCube GPU, audio
DSP, AM-Baseboard, AM-Mediaboard), so the game runs as a normal native program, with room for
modern features such as higher resolutions, widescreen and remappable controls.

> **Status: early development, not playable yet.** Milestone 1 is done: the recompiler, the CPU
> runtime and the test suite. Next is milestone 2, running the recompiler on the real game. See the
> [roadmap](#roadmap).

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
| Games | `games/` | Per-game symbol maps, recompiler config and patches (from milestone 2) |
| Tests | `tests/` | Decoder tests, known-answer semantics tests, and differential tests (interpreter vs recompiled code) |

Details are in [docs/architecture.md](docs/architecture.md). Notes on the arcade hardware are in
[docs/triforce.md](docs/triforce.md).

## Building

You need CMake 3.25+, Ninja, and GCC 12+ or Clang 16+. On Windows, use clang-cl: the recompiled
code relies on weak symbols, which MSVC does not support.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Right now this builds the recompiler, the CPU runtime and the test suite. The tests need no game
data: they generate their own PowerPC test programs.

## Legal

This repository contains **no game code, data or assets**, and none may ever be added. That
includes the recompiler's output, because it is derived from the game.

To play, you will need your own copy of the game:
- Put your images in `private/`, for example `private/GGPE02.iso`. The folder is gitignored.
- The build and the executable read the images from there.
- [docs/game-files.md](docs/game-files.md) explains how to prepare them.

Mario Kart is a trademark of Nintendo. This project is not affiliated with Nintendo, Namco
(Bandai Namco) or Sega.

OpenGP is licensed under the **GPL-3.0** (see [LICENSE](LICENSE)). Parts of the runtime are ported
from the [Dolphin emulator](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) and are
marked as such in the source. Vendored third-party code in `thirdparty/` keeps its own license.

## Roadmap

| # | Milestone | Needs the game |
|---|-----------|----------------|
| M1 | Scaffolding, recompiler, CPU runtime, test suite, CI (**done**) | no |
| M2 | MKAGP2 (GGPE02) analysis: symbol map, SDK functions identified, whole game recompiles and builds | yes |
| M3 | Boot to the main loop: OS HLE, media board, JVS stubs, VI retrace, hardware-check patches | yes |
| M4 | Graphics: GX FIFO decoder, TEV ubershader, first frames, attract mode | yes |
| M5 | Input and gameplay: JVS mapping, free play, full race, wheel force feedback | yes |
| M6 | Audio (DSP AX HLE) | yes |
| M7 | Card saves, settings menu, resolution scaling, widescreen | yes |
| M8 | Mario Kart Arcade GP (GGPE01) | yes |

## Acknowledgements

- **Dolphin**: the reference for Gekko, GameCube and Triforce behaviour.
- **decomp-toolkit**: GameCube binary analysis.
- **N64Recomp / Zelda64Recomp** and **XenonRecomp / UnleashedRecomp**: the overall design.
