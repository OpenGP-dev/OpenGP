#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Name SDK functions using Dolphin's signature database.

Dolphin's totaldb.dsy maps a checksum of a function's instruction opcodes
(immediates and addresses masked out) plus its size to a name. This script
computes the same checksum for every function in a decomp-toolkit
symbols.txt and reports the matches. With --apply it renames functions that
still have generated names (fn_XXXXXXXX), when the match is unambiguous:

  * the size matches the database entry exactly,
  * no other function in the game has the same checksum and size, and
  * the name is not already used by another function.

    python tools/sigmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt [--apply]

The database is downloaded from Dolphin's repository into build/tools/ if needed.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DSY_URL = "https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Data/Sys/totaldb.dsy"
GENERATED_NAME = re.compile(r"^fn_[0-9A-F]{8}$")
# The Nintendo SDK, MSL and TRK are C. The database also holds C++ functions
# from other games, whose short methods produce false matches; skip those.
C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
SYMBOL_LINE = re.compile(r'^(?P<name>"[^"]*"|\S+) = (?P<sec>[^:;\s]+):0x(?P<addr>[0-9A-Fa-f]+);(?P<rest>.*)$')


def load_dsy(path: Path) -> dict[int, tuple[int, str]]:
    data = path.read_bytes()
    (count,) = struct.unpack_from("<I", data, 0)
    db: dict[int, tuple[int, str]] = {}
    for i in range(count):
        checksum, size, raw = struct.unpack_from("<II128s", data, 4 + i * 136)
        db[checksum] = (size, raw.split(b"\0", 1)[0].decode("ascii", "replace"))
    return db


class Dol:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        self.sections = []
        for i in range(18):
            off, addr, size = (struct.unpack_from(">I", self.data, base + 4 * i)[0] for base in (0x00, 0x48, 0x90))
            if size:
                self.sections.append((addr, size, off))

    def word(self, addr: int) -> int | None:
        for base, size, off in self.sections:
            if base <= addr < base + size:
                return struct.unpack_from(">I", self.data, off + addr - base)[0]
        return None


def checksum(dol: Dol, start: int, size: int) -> int | None:
    """Dolphin's HashSignatureDB::ComputeCodeChecksum over [start, start + size)."""
    total = 0
    for addr in range(start, start + size, 4):
        opcode = dol.word(addr)
        if opcode is None:
            return None
        op = opcode & 0xFC000000
        op2 = op3 = 0
        aux = op >> 26
        if aux == 4:
            op2 = opcode & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = opcode & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = opcode & 0x7FF
        elif aux == 59:
            op2 = opcode & 0x3F
            if op2 < 16:
                op3 = opcode & 0x7C0
        elif 32 <= aux < 56:
            op2 = opcode & 0x03FF0000
        total = ((total << 17) & 0xFFFE0000) | ((total >> 15) & 0x0001FFFF)
        total ^= op | op2 | op3
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dol", type=Path)
    parser.add_argument("symbols", type=Path)
    parser.add_argument("--dsy", type=Path, default=ROOT / "build" / "tools" / "totaldb.dsy")
    parser.add_argument("--apply", action="store_true", help="rename matched fn_XXXXXXXX functions in place")
    parser.add_argument("--min-size", type=lambda s: int(s, 0), default=0x10,
                        help="ignore matches for functions smaller than this (default 0x10)")
    args = parser.parse_args()

    if not args.dsy.exists():
        args.dsy.parent.mkdir(parents=True, exist_ok=True)
        print(f"Downloading {DSY_URL}")
        urllib.request.urlretrieve(DSY_URL, args.dsy)
    db = load_dsy(args.dsy)
    dol = Dol(args.dol)

    lines = args.symbols.read_text(encoding="utf-8").splitlines()
    functions = []  # (line index, name, address, size)
    used_names = set()
    for i, line in enumerate(lines):
        m = SYMBOL_LINE.match(line)
        if not m:
            continue
        name = m["name"].strip('"')
        used_names.add(name)
        if "type:function" not in m["rest"]:
            continue
        size_m = re.search(r"size:0x([0-9A-Fa-f]+)", m["rest"])
        if size_m:
            functions.append((i, name, int(m["addr"], 16), int(size_m[1], 16)))

    # Group game functions by (checksum, size) to detect ambiguity.
    by_key: dict[tuple[int, int], list] = defaultdict(list)
    for fn in functions:
        c = checksum(dol, fn[2], fn[3])
        if c is not None:
            by_key[(c, fn[3])].append(fn)

    renames: dict[int, str] = {}
    agree = disagree = ambiguous = 0
    for (c, size), fns in by_key.items():
        entry = db.get(c)
        if not entry or entry[0] != size or size < args.min_size:
            continue
        sig_name = entry[1]
        # __sinit_<file>_cpp are per-file static initialisers: a shape match
        # with another game's file says nothing about this one.
        if not C_IDENTIFIER.match(sig_name) or sig_name.startswith("__sinit_"):
            continue
        if len(fns) > 1:
            ambiguous += len(fns)
            continue
        idx, name, addr, _ = fns[0]
        if GENERATED_NAME.match(name):
            if sig_name in used_names:
                continue
            renames[idx] = sig_name
            used_names.add(sig_name)
        elif name == sig_name:
            agree += 1
        else:
            disagree += 1
            print(f"  note: {addr:08X} is named {name}, signature says {sig_name}")

    print(f"{len(functions)} functions, {len(db)} signatures")
    print(f"already named and confirmed: {agree}; named differently: {disagree}; ambiguous (skipped): {ambiguous}")
    print(f"new names: {len(renames)}")
    for idx in sorted(renames, key=lambda i: lines[i]):
        m = SYMBOL_LINE.match(lines[idx])
        print(f"  {int(m['addr'], 16):08X} {m['name']} -> {renames[idx]}")

    if args.apply and renames:
        for idx, new_name in renames.items():
            m = SYMBOL_LINE.match(lines[idx])
            rest = m["rest"]
            if "scope:" not in rest:
                rest += " scope:global"
            lines[idx] = f"{new_name} = {m['sec']}:0x{m['addr']};{rest}"
        # newline="\n": keep LF line endings on Windows (decomp-toolkit writes LF).
        args.symbols.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        print(f"updated {args.symbols}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
