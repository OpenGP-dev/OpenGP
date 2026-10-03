# OpenGP

OpenGP is a work-in-progress **native PC port of Mario Kart Arcade GP and Mario Kart Arcade GP 2**
(Namco, 2005 and 2007, Triforce arcade hardware). It is built by **static recompilation**, the
same approach as [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp).

The game's PowerPC (Gekko) machine code is translated into C++ ahead of time. That C++ is then
compiled together with a runtime that stands in for the Triforce hardware (GameCube GPU, audio
DSP, AM-Baseboard, AM-Mediaboard), so the game runs as a normal native program, with room for
modern features such as higher resolutions, widescreen and remappable controls.

> **Status: playable.** Milestones 1 to 7 are done. Mario Kart Arcade GP 2 (7,612 functions)
> recompiles, boots from its disc image through the emulated Triforce hardware (media board,
> baseboard, JVS I/O, card reader) and plays in a window or fullscreen (Vulkan), in 4:3 or
> widescreen and at up to 8 times its resolution: insert a coin (or turn on free play), make a
> Mario Card, pick a character and cup, and race at 60 fps with the keyboard, a gamepad or a
> steering wheel with force feedback, with the game's music and sound effects (its DSP audio
> microcode is emulated). A menu (Esc) has the settings, the controls and the Mario Cards; the
> game's own test menu works too. See [docs/playing.md](docs/playing.md) and the
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
| Machine | `runtime/hw/`, `runtime/os/`, `runtime/triforce/`, `runtime/gx/` | Hardware models (interrupts, timers, VI, DSP, AI, SI, EXI, DI), boot and guest threads, the Triforce boards, and the GPU (command processor, vertex loading and transform, textures, EFB copies) |
| Renderer | `runtime/video/` | SDL3 GPU renderer: the TEV ubershader, EFB, presenting |
| Input | `runtime/input/` | Keyboard, gamepads and wheels (SDL3) mapped onto the cabinet's controls, force feedback, scripted input |
| Launcher | `runtime/app/` | Command line, settings, window, the in-game menu (Dear ImGui) |
| Games | `games/` | Per-game symbol maps, recompiler config, patches and launcher |
| Scripts | `tools/*.py`, `tools/devshell.ps1` | Game setup (extract and verify `main.dol`), SDK signature matching, Windows build shell |
| Tests | `tests/` | Decoder tests, known-answer semantics tests, and differential tests (interpreter vs recompiled code) |

Details are in [docs/architecture.md](docs/architecture.md). Notes on the arcade hardware are in
[docs/triforce.md](docs/triforce.md).

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

Mario Kart is a trademark of Nintendo. This project is not affiliated with Nintendo, Namco
(Bandai Namco) or Sega.

OpenGP is licensed under the **GPL-3.0** (see [LICENSE](LICENSE)). Parts of the runtime are ported
from the [Dolphin emulator](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) and are
marked as such in the source. Vendored third-party code in `thirdparty/` keeps its own license.

## Roadmap

| # | Milestone | Needs the game |
|---|-----------|----------------|
| M1 | Scaffolding, recompiler, CPU runtime, test suite, CI (**done**) | no |
| M2 | MKAGP2 (GGPE02) analysis: symbol map, SDK functions identified, whole game recompiles and builds (**done**) | yes |
| M3 | Boot to the main loop: guest threads, hardware models, media board, baseboard and JVS, card reader, GPU FIFO (**done**) | yes |
| M4 | Graphics: vertex loader, textures, TEV ubershader, first frames, attract mode; hardware-check patches as needed (**done**) | yes |
| M5 | Input and gameplay: JVS mapping, free play, full race, wheel force feedback (**done**) | yes |
| M6 | Audio: the DSP's AX microcode (driven by MusyX) at a high level, sound output (**done**) | yes |
| M7 | Card saves, settings menu, resolution scaling, widescreen, fullscreen; the game's test menu (**done**) | yes |
| M8 | Mario Kart Arcade GP (GGPE01) | yes |

## Acknowledgements

- **Dolphin**: the reference for Gekko, GameCube and Triforce behaviour.
- **decomp-toolkit**: GameCube binary analysis.
- **N64Recomp / Zelda64Recomp** and **XenonRecomp / UnleashedRecomp**: the overall design.
