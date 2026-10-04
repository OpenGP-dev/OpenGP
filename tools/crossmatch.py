#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the same functions in two games by their code.

Mario Kart Arcade GP and GP 2 share their engine, the arcade library and the
SDK, at different addresses. This compares every function in two
decomp-toolkit symbol lists by size and by instruction words with the fields
that change between builds masked out (branch displacements and the
immediates of addi/addis/ori/oris and of loads and stores), and prints where
functions of the first game are in the second.

    python tools/crossmatch.py A.dol A_symbols.txt B.dol B_symbols.txt [ADDRESS ...]

With addresses (functions of A, in hex), prints each one's matches in B.
Without, prints how many functions match one to one.

    python tools/crossmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt \\
                               orig/GGPE01/sys/main.dol games/mkgp1/config/symbols.txt 80289B54
"""

from __future__ import annotations

import re
import struct
import sys
from collections import defaultdict

SYMBOL = re.compile(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")


class Dol:
    def __init__(self, path: str):
        self.data = open(path, "rb").read()
        offsets = struct.unpack(">18I", self.data[0:72])
        addresses = struct.unpack(">18I", self.data[72:144])
        sizes = struct.unpack(">18I", self.data[144:216])
        self.sections = [(a, o, s) for o, a, s in zip(offsets, addresses, sizes) if s]

    def read(self, address: int, size: int) -> bytes | None:
        for a, o, s in self.sections:
            if a <= address and address + size <= a + s:
                return self.data[o + address - a:o + address - a + size]
        return None


def load_functions(path: str) -> list[tuple[int, int, str]]:
    functions = []
    with open(path) as f:
        for line in f:
            m = SYMBOL.match(line)
            if m:
                functions.append((int(m[2], 16), int(m[3], 16), m[1]))
    return functions


def masked(code: bytes) -> tuple[int, ...]:
    words = []
    for (w,) in struct.iter_unpack(">I", code):
        op = w >> 26
        if op == 18:  # b, bl
            w &= 0xFC000003
        elif op in (14, 15, 24, 25) or 32 <= op <= 55:  # addi, addis, ori, oris, loads, stores
            w &= 0xFFFF0000
        words.append(w)
    return tuple(words)


def index(dol: Dol, functions: list[tuple[int, int, str]]):
    by_key: dict[tuple, list[tuple[int, str]]] = defaultdict(list)
    key_of: dict[int, tuple] = {}
    for address, size, name in functions:
        code = dol.read(address, size)
        if code is None:
            continue
        key = (size, masked(code))
        by_key[key].append((address, name))
        key_of[address] = key
    return by_key, key_of


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(__doc__)
        return 2
    functions_a = load_functions(argv[1])
    functions_b = load_functions(argv[3])
    by_key_a, key_of_a = index(Dol(argv[0]), functions_a)
    by_key_b, _ = index(Dol(argv[2]), functions_b)
    if len(argv) > 4:
        names = {address: name for address, _, name in functions_a}
        for text in argv[4:]:
            address = int(text, 16)
            key = key_of_a.get(address)
            matches = by_key_b.get(key, []) if key else []
            found = ", ".join(f"{a:08X} {n}" for a, n in matches) or "none"
            print(f"{address:08X} {names.get(address, '?')} -> {found}")
        return 0
    unique = sum(1 for k, v in by_key_a.items() if len(v) == 1 and len(by_key_b.get(k, [])) == 1)
    print(f"{len(functions_a)} and {len(functions_b)} functions; {unique} match one to one")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
