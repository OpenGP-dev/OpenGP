# Game files

OpenGP never contains anything from the games. You supply your own images; everything derived from
them stays on your machine.

## Supported releases

| Game | ID | `main.dol` SHA-1 |
|------|----|------------------|
| Mario Kart Arcade GP (USA) | `GGPE01` | `0e84df7cac27eab7d6dbb1892933ae3bf762f742` |
| Mario Kart Arcade GP 2 (USA) | `GGPE02` | `2969c177219dda71ad36b41575bede7ad4637f96` |

## Preparing an image

OpenGP reads plain disc images, `.iso` or `.gcm`, the kind Dolphin runs.

- A compressed image (`.rvz`, `.gcz`, `.wia`, `.ciso`) must be converted first: in Dolphin,
  right-click the game, choose **Convert File** and pick the ISO format.
- Raw NAND dumps must be converted first with
  [triforce-nand-iso-extract](https://github.com/FIX94/triforce-nand-iso-extract).
- [triforce-header-patcher](https://github.com/FIX94/triforce-header-patcher) checks images and
  fixes known bad headers.
- Some converted dumps have a placeholder header (ID `RELSAB`, "Sample Game Name"). OpenGP
  recognises them by their `main.dol` and treats them as the right game, so they need no fixing.

In an installer package, OpenGP builds the games from your images: add them in OpenGP, or put them
in the package's `ISO` folder (see [packaging.md](packaging.md)). The rest of this page is for
source builds.

## Setting up a source build

1. Put your images in `ISO/` or `private/` at the repository root. Both folders are gitignored.
2. Run:
   ```sh
   python tools/setup.py
   ```
   This extracts each game's executable to `orig/<ID>/sys/main.dol` (gitignored) and checks it
   against the table above; an executable of another revision is reported and not written. Then it
   downloads the pinned [decomp-toolkit](https://github.com/encounter/decomp-toolkit) into
   `build/tools/` and checks its SHA-256, for regenerating the symbol lists. `--no-dtk` skips the
   download, and the games build without it.
3. Configure and build as usual (see [building.md](building.md)). CMake builds each game whose
   `main.dol` is present and has the expected hash, and skips the others.
4. Run the game (`opengp-mkgp2` or `opengp-mkgp1`). It finds the image in `ISO/` or `private/` (or
   takes `--iso`), and runs only if the image holds the same `main.dol` the build was made from; any
   other image (a different game, region or revision) is refused with its hash.

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

The game's own functions and globals are named from
[mkgp2-decomp](https://github.com/naari3/mkgp2-decomp) (CC0), a decompilation of the Japanese
release of GP 2 (`GNLJ82`). Its code and data are the same as `GGPE02`'s apart from a few
functions and objects of other sizes, so `tools/decompnames.py` lines up the two symbol lists by
the sizes of their functions and, section by section, of their objects, and takes the decomp's
names. Where the decomp has split an object to name a field of it (`g_DebugPrintfEnable` in the
settings block), the same object here is split the same way. With a clone of the decomp in
`gp2decomp/` (gitignored):

```sh
python tools/decompnames.py gp2decomp/mkgp2-decomp/config/GNLJ82/symbols.txt games/mkgp2/config/symbols.txt --apply
```

GP has no decomp. `tools/crossmatch.py --apply` gives it GP 2's names for the functions whose
code is the same in both games, and for the globals those functions use at the same places in
their code:

```sh
python tools/crossmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt \
                           orig/GGPE01/sys/main.dol games/mkgp1/config/symbols.txt --apply
```

Both only rename generated names (`fn_XXXXXXXX`, `dtor_XXXXXXXX`, `lbl_XXXXXXXX`), and keep
names that are already taken; run them again as the decomp names more. Renaming objects does not
change the generated code. Splitting one does: the recompiler bounds jump tables by their
objects' sizes, so rebuild and play after a split. The decomp's names are its authors'
reading of the code, so they are a guide rather than a fact. Where it names an SDK function
differently, the name from `sigmatch.py` is kept.

## Working with Claude Code

Cloud sessions (claude.ai/code) start from a fresh clone of the GitHub repository, so they cannot
see your images. Do game-specific work in a local session, in a clone that has `ISO/` or
`private/`. A cloud session can be pulled down with `claude --teleport <session-id>`.
