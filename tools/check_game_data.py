#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail if a tracked file looks like it came from the games.

OpenGP never contains game code, data or assets, nor the recompiler's output.
CI runs this on every push and pull request; run it yourself before committing:

    python3 tools/check_game_data.py

It checks every file `git ls-files` lists:
- no disc images, executables or other game file types, and nothing in the
  folders that hold game data (ISO/, private/, orig/, user*/, build/ ...);
- no binary files, except the free replacement fonts in thirdparty/gc-fonts and
  the menu's typeface in thirdparty/fonts (M PLUS Rounded 1c, SIL OFL 1.1);
- no file over 1 MiB, except the symbol maps and vendored headers;
- no file identical to a supported game's main.dol (its SHA-1 is in
  games/*/game.toml).
"""
import fnmatch
import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

GAME_TYPES = ("*.iso", "*.gcm", "*.dol", "*.rel", "*.img", "*.nand", "*.bin", "*.gdi", "*.chd",
              "*.rvz", "*.wbfs", "*.ciso", "*.gcz", "*.wia", "*.tgc", "*.elf", "*.exe", "*.dll",
              "*.so", "*.dylib", "*.a", "*.lib", "*.o", "*.obj", "*.zip", "*.7z", "*.rar", "*.wav",
              "*.png", "*.jpg", "*.jpeg", "*.tpl", "*.bmp")
GAME_FOLDERS = ("iso/*", "private/*", "orig/*", "user*/*", "build/*", "out/*", "generated/*", "gp2decomp/*", "ref/*")
ALLOWED_BINARY = ("thirdparty/gc-fonts/*.bin", "thirdparty/fonts/*/*.ttf")
ALLOWED_LARGE = ("games/*/config/symbols.txt", "thirdparty/*")
MAX_SIZE = 1 << 20


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True, capture_output=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def matches(path, patterns):
    return any(fnmatch.fnmatch(path, p) for p in patterns)


def supported_dol_hashes():
    hashes = {}
    for manifest in sorted(ROOT.glob("games/*/game.toml")):
        m = re.search(r'^\s*dol_sha1\s*=\s*"([0-9a-fA-F]{40})"', manifest.read_text(encoding="utf-8"), re.M)
        if m:
            hashes[m.group(1).lower()] = manifest.parent.name
    return hashes


def main():
    dols = supported_dol_hashes()
    problems = []
    for path in tracked_files():
        file = ROOT / path
        if not file.is_file():
            continue  # deleted in the working tree
        lower = path.lower()
        if matches(lower, GAME_FOLDERS):
            problems.append(f"{path}: in a folder that holds game data or build output")
            continue
        if matches(lower, GAME_TYPES) and not matches(path, ALLOWED_BINARY):
            problems.append(f"{path}: a file type that can hold game code or data")
            continue
        data = file.read_bytes()
        if b"\0" in data[:8192] and not matches(path, ALLOWED_BINARY):
            problems.append(f"{path}: a binary file")
        if len(data) > MAX_SIZE and not matches(path, ALLOWED_LARGE):
            problems.append(f"{path}: larger than 1 MiB ({len(data)} bytes)")
        digest = hashlib.sha1(data).hexdigest()
        if digest in dols:
            problems.append(f"{path}: identical to {dols[digest]}'s main.dol")
    if problems:
        print("These tracked files look like game data, which OpenGP must never contain:")
        for p in problems:
            print("  " + p)
        print("Remove them from the repository (see README.md, Legal). If a file is fine, add it to the "
              "allow lists in tools/check_game_data.py with a reason.")
        return 1
    print(f"OK: no game data in {len(tracked_files())} tracked files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
