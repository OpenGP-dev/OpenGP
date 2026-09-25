# Game files

OpenGP never contains anything from the games. You supply your own images; everything derived from
them stays on your machine.

## Where to put them

Put Dolphin-ready images in `private/` at the repository root. The whole folder is gitignored.

```
private/
  GGPE02.iso        Mario Kart Arcade GP 2
  GGPE01.iso        Mario Kart Arcade GP
```

If you have raw NAND dumps rather than `.iso` files:
- Convert them with [triforce-nand-iso-extract](https://github.com/FIX94/triforce-nand-iso-extract).
- Check or fix bad headers with [triforce-header-patcher](https://github.com/FIX94/triforce-header-patcher).

## What the build needs

Only `main.dol`, the game's executable (a few MB), is needed to recompile. You can extract it with
Dolphin:

1. Right-click the game and choose *Properties* → *Filesystem*.
2. Right-click the disc and choose *Extract System Data*.
3. Copy `sys/main.dol` to `private/GGPE02/main.dol`.

An extraction tool will be added in milestone 2, so this step becomes part of the build.

## Working with Claude Code

Cloud sessions (claude.ai/code) start from a fresh clone of the GitHub repository, so they cannot
see your `private/` folder.

**For game-specific work (milestone 2 onward):**
- Best: run Claude Code locally (desktop app or CLI) in your own clone, where `private/` exists.
- Alternatively: upload just `main.dol` into a cloud session's `private/` folder when a task needs
  it.

Either way, only address/symbol configuration files (`games/*/config/`, `recomp.toml`) are ever
committed.
