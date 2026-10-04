# Game files

OpenGP never contains anything from the games. You supply your own images; everything derived from
them stays on your machine.

## Supported releases

| Game | ID | `main.dol` SHA-1 |
|------|----|------------------|
| Mario Kart Arcade GP (USA) | `GGPE01` | `0e84df7cac27eab7d6dbb1892933ae3bf762f742` |
| Mario Kart Arcade GP 2 (USA) | `GGPE02` | `2969c177219dda71ad36b41575bede7ad4637f96` |

## Setting up

1. Put your Dolphin-ready images (`.iso` / `.gcm`) in `ISO/` or `private/` at the repository root.
   Both folders are gitignored.
   - Raw NAND dumps must be converted first with
     [triforce-nand-iso-extract](https://github.com/FIX94/triforce-nand-iso-extract).
   - [triforce-header-patcher](https://github.com/FIX94/triforce-header-patcher) checks images and
     fixes known bad headers.
2. Run:
   ```sh
   python tools/setup.py
   ```
   This downloads the pinned [decomp-toolkit](https://github.com/encounter/decomp-toolkit) into
   `build/tools/`. It then extracts each game's executable to `orig/<ID>/sys/main.dol` (gitignored)
   and checks it against the table above.
3. Configure and build as usual (see [building.md](building.md)). CMake builds each game whose
   `main.dol` is present and has the expected hash, and skips the others.

## What gets committed

Only files that describe the game without containing it:
- `games/<game>/config/`: the decomp-toolkit config, plus `symbols.txt` / `splits.txt` with
  addresses and names.
- `games/<game>/recomp.toml`: recompiler settings, patches and hooks.
- Hand-written C++ (patches, HLE).

The recompiled C++ is generated at build time under `build/`, because it is derived from the game.

## Regenerating the symbol list

Each game's `games/<game>/config/symbols.txt` comes from decomp-toolkit's analysis (`mkgp1` is
`GGPE01`, `mkgp2` is `GGPE02`). To refresh it, run from the repository root:

```sh
build/tools/dtk dol split games/mkgp2/config/config.yml build/GGPE02
```

The same command also writes disassembly to `build/GGPE02/asm/`, which is useful for reading the
game's code.

To name more SDK functions from Dolphin's signature database (`totaldb.dsy`, downloaded
automatically):

```sh
python tools/sigmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt           # report
python tools/sigmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt --apply   # rename
```

It only renames generated `fn_XXXXXXXX` names, and only when the match is unambiguous. A few SDK
functions the runtime replaces are named by hand afterwards (see [triforce.md](triforce.md)):
`OSSetSaveRegion`, `OSGetSaveRegion`, `OSGetSavedRegion`, and `OSGetExecParams` (decomp-toolkit
calls it `__OSGetExecParams`).

## Working with Claude Code

Cloud sessions (claude.ai/code) start from a fresh clone of the GitHub repository, so they cannot
see your images. Do game-specific work in a local session, in a clone that has `ISO/` or
`private/`. A cloud session can be pulled down with `claude --teleport <session-id>`.
