# Contributing to OpenGP

OpenGP is feature-locked at 1.0: it takes bug fixes and design tweaks (wording, layout, how
something looks), not new features. Bug reports and fixes are welcome. Before a larger fix, open an
issue so we can agree on the approach.

## The one rule: no game data

OpenGP contains **no game code, data or assets**, and none may ever be added. That rules out:

- disc images, `main.dol` or any other file from the games, whole or in part;
- the recompiler's output (`build/.../generated/*.cpp`), which is derived from the game;
- the games' textures, models, sounds, music, fonts or text, and screenshots, captures or audio
  dumps of them;
- code copied from projects that have no licence (such as mkgp2-patch). Facts, such as addresses,
  sizes and what a function does, are fine.

Addresses, symbol names and sizes in `games/*/config/symbols.txt` and hand-written code that names
them are fine. CI runs `tools/check_game_data.py` on every push and pull request; run it yourself
before committing:

```sh
python3 tools/check_game_data.py
```

When reporting a bug, never attach game files. `user/hang-report.txt`, a crash report and the
console output are what helps.

## Licence

OpenGP is GPL-3.0-or-later. By contributing you agree that your contribution is licensed the same
way. Start new source files with:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

Code ported from Dolphin (GPL-2.0-or-later) is welcome. Say so at the top of the file, naming the
Dolphin files, as the existing ports do:

```cpp
// Based on Dolphin's VideoInterface.cpp (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
```

Other third-party code needs a compatible licence and an entry in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Building and testing

[docs/building.md](docs/building.md) explains how to build. The test suite needs no game data:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Add a test for what you change where you can. For changes to what the games do, also play the
part of the game you changed; say in the pull request what you tested and on which system.

## Code and commits

- Follow the style of the code around your change (C++20, two-space indent, about 110 columns).
- Keep commits focused, with a message that says what changes for the player or the developer.
- OpenGP is largely written with AI. Commits made with an AI tool end with a `Co-Authored-By:`
  line naming it, so the history shows which ones; please do the same.
