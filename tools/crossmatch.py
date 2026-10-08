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
Without, prints how many functions match one to one. With --apply, gives
A's names to B's functions that match one to one and still have generated
names (fn_XXXXXXXX, dtor_XXXXXXXX), when B has no symbol of that name yet.
It names B's objects (lbl_XXXXXXXX) the same way: those functions reach
data at the same places in their code (through r2, r13 or lis), which pairs
an object of A with one of B when every such place agrees. An address of A
at the end of a name becomes B's (or is dropped).

    python tools/crossmatch.py orig/GGPE02/sys/main.dol games/mkgp2/config/symbols.txt \\
                               orig/GGPE01/sys/main.dol games/mkgp1/config/symbols.txt 80289B54
"""

from __future__ import annotations

import re
import struct
import sys
from collections import defaultdict

SYMBOL = re.compile(r"^(\S+) = \.\w+:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
SYMBOL_LINE = re.compile(r'^(?P<name>"[^"]*"|\S+) = (?P<sec>[^:;\s]+):0x(?P<addr>[0-9A-Fa-f]+);(?P<rest>.*)$')
GENERATED = re.compile(r"^(fn|dtor|lbl)_[0-9A-F]{8}$")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ADDRESS_SUFFIX = re.compile(r"^(?P<base>\w+?)_(?P<hex>[0-9A-F]{8})$")


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


def small_data_bases(dol: Dol, functions: list[tuple[int, int, str]]) -> dict[int, int]:
    """r2 and r13, as __init_registers sets them (lis, then ori)."""
    values: dict[int, int] = {}
    for address, size, name in functions:
        if name != "__init_registers":
            continue
        for (w,) in struct.iter_unpack(">I", dol.read(address, size)):
            op, rs, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:  # lis rD, imm
                values[rs] = imm << 16
            elif op == 24 and rs in values:  # ori rA, rS, imm
                values[ra] = values[rs] | imm
    return {r: values[r] for r in (2, 13) if r in values}


def data_references(dol: Dol, address: int, size: int, bases: dict[int, int]) -> dict[int, int]:
    """The addresses the function's loads, stores and addi reach through r2,
    r13 or a register set by lis, by instruction index."""
    refs, high = {}, {}
    for i, (w,) in enumerate(struct.iter_unpack(">I", dol.read(address, size))):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:  # lis
            high[rd] = imm << 16
            continue
        if op == 14 or 32 <= op <= 55:  # addi, loads, stores
            base = bases.get(ra) if ra in (2, 13) else high.get(ra)
            if base is not None:
                refs[i] = (base + imm - (0x10000 if imm & 0x8000 else 0)) & 0xFFFFFFFF
            if op == 14 or op <= 35 or 40 <= op <= 43 or 48 <= op <= 51:  # writes rD
                high.pop(rd, None)
    return refs


def apply_names(pairs, dols, functions, symbols) -> None:
    """Gives B's generated names A's names: the functions in `pairs` (A's
    address and name, B's), and the objects they reach at the same places in
    their code, when every such place agrees."""
    bases = [small_data_bases(dol, fns) for dol, fns in zip(dols, functions)]
    objects: list[dict[int, str]] = [{}, {}]
    for side, path in enumerate(symbols):
        for line in open(path, encoding="utf-8"):
            m = SYMBOL_LINE.match(line)
            if m and "type:object" in m["rest"]:
                objects[side][int(m["addr"], 16)] = m["name"].strip('"')
    seen: dict[int, set[int]] = defaultdict(set)  # A's object -> B's addresses at the same places
    sizes = [{addr: s for addr, s, _ in fns} for fns in functions]
    for (a, _), (b, _) in pairs:
        refs_a = data_references(dols[0], a, sizes[0][a], bases[0])
        refs_b = data_references(dols[1], b, sizes[1][b], bases[1])
        for i, target in refs_a.items():
            if target in objects[0] and i in refs_b:
                seen[target].add(refs_b[i])
    claimed: dict[int, set[int]] = defaultdict(set)  # B's object -> A's objects that reach it
    for target, places in seen.items():
        if len(places) == 1:
            claimed[next(iter(places))].add(target)
    object_pairs = [(next(iter(a)), b) for b, a in claimed.items() if len(a) == 1 and b in objects[1]]

    address_map = {a: b for (a, _), (b, _) in pairs} | {a: b for a, b in object_pairs}
    renames = [(address, name) for (_, name), (address, _) in pairs]
    renames += [(b, objects[0][a]) for a, b in object_pairs]
    names_b = {**{address: name for address, _, name in functions[1]}, **objects[1]}
    lines = open(symbols[1], encoding="utf-8").read().splitlines()
    used, line_of = set(), {}
    for i, line in enumerate(lines):
        m = SYMBOL_LINE.match(line)
        if m:
            used.add(m["name"].strip('"'))
            line_of.setdefault(int(m["addr"], 16), i)
    renamed = [0, 0]
    for address, name in renames:
        if GENERATED.match(name) or not IDENTIFIER.match(name) or not GENERATED.match(names_b[address]):
            continue
        m = ADDRESS_SUFFIX.match(name)
        if m and int(m["hex"], 16) in address_map:  # names ending in an address of A (Stub_NoOp_8012161C)
            name = f"{m['base']}_{address_map[int(m['hex'], 16)]:08X}"
        elif m:
            name = f"{m['base']}_{address:08X}"
        if name in used:
            continue
        m = SYMBOL_LINE.match(lines[line_of[address]])
        lines[line_of[address]] = f"{name} = {m['sec']}:0x{m['addr']};{m['rest']}"
        used.add(name)
        renamed["type:object" in m["rest"]] += 1
    # newline="\n": keep LF line endings on Windows (decomp-toolkit writes LF).
    with open(symbols[1], "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"named {renamed[0]} functions and {renamed[1]} objects in {symbols[1]}")


def main(argv: list[str]) -> int:
    apply = "--apply" in argv
    argv = [a for a in argv if a != "--apply"]
    if len(argv) < 4:
        print(__doc__)
        return 2
    functions_a = load_functions(argv[1])
    functions_b = load_functions(argv[3])
    dol_a, dol_b = Dol(argv[0]), Dol(argv[2])
    by_key_a, key_of_a = index(dol_a, functions_a)
    by_key_b, _ = index(dol_b, functions_b)
    if apply:
        pairs = [(v[0], by_key_b[k][0]) for k, v in by_key_a.items() if len(v) == 1 and len(by_key_b.get(k, [])) == 1]
        apply_names(pairs, (dol_a, dol_b), (functions_a, functions_b), (argv[1], argv[3]))
        return 0
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
