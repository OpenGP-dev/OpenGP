#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Name functions using a decompilation of another release of the game.

naari3/mkgp2-decomp (CC0) names Mario Kart Arcade GP 2's functions in the
Japanese release, GNLJ82. Its code is the same as GGPE02's apart from a
handful of functions of other sizes, which shift everything after them. This
lines up the two symbol lists by the sizes of their functions, in address
order, and prints the decomp's names for this game's functions. With --apply
it renames functions that still have generated names (fn_XXXXXXXX,
dtor_XXXXXXXX):

  * runs of functions whose sizes agree are paired in order; between two
    runs, the functions left over are paired in order when both releases
    have the same number (the ones whose size differs);
  * a name is skipped if another symbol already has it, or if hand-written
    code (games/, runtime/) defines a function or hook of that name;
  * an address at the end of a decomp name (Stub_NoOp_80121c34) is the
    other release's: it becomes this release's address when it is a
    function's, and is dropped otherwise (a data address).

    python tools/decompnames.py gp2decomp/mkgp2-decomp/config/GNLJ82/symbols.txt \\
                                games/mkgp2/config/symbols.txt [--apply]

Only functions are named. The decomp's names are its authors' reading of the
code; the SDK names from tools/sigmatch.py are kept where the two differ.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOL_LINE = re.compile(r'^(?P<name>"[^"]*"|\S+) = (?P<sec>[^:;\s]+):0x(?P<addr>[0-9A-Fa-f]+);(?P<rest>.*)$')
SIZE = re.compile(r"size:0x([0-9A-Fa-f]+)")
# Names decomp-toolkit (or Ghidra, in the decomp) makes up.
GENERATED = re.compile(r"^(fn|dtor|ctor|lbl|func|FUN|thunk_FUN)_[0-9A-Fa-f]{8}$")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# A trailing address: 5 to 8 hex digits, at least one of them a digit (so
# words such as "_Added" are left alone).
ADDRESS_SUFFIX = re.compile(r"^(?P<base>\w+?)_(?P<hex>(?=[0-9A-Fa-f]*[0-9])[0-9A-Fa-f]{5,8})$")
HANDWRITTEN = re.compile(r'PPC_(?:FUNC|HOOK|EXTERN_FUNC)\((\w+)\)|^\s*function\s*=\s*"(\w+)"', re.M)


class Function:
    def __init__(self, line: int, name: str, address: int, size: int):
        self.line, self.name, self.address, self.size = line, name, address, size


def load(path: Path) -> tuple[list[str], list[Function], set[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    functions, names = [], set()
    for i, line in enumerate(lines):
        m = SYMBOL_LINE.match(line)
        if not m:
            continue
        name = m["name"].strip('"')
        names.add(name)
        size = SIZE.search(m["rest"])
        if "type:function" in m["rest"] and size:
            functions.append(Function(i, name, int(m["addr"], 16), int(size[1], 16)))
    functions.sort(key=lambda f: f.address)
    return lines, functions, names


def handwritten_names() -> set[str]:
    names = set()
    for top in ("games", "runtime"):
        for path in (ROOT / top).rglob("*"):
            if path.suffix in (".cpp", ".h", ".toml"):
                for m in HANDWRITTEN.finditer(path.read_text(encoding="utf-8", errors="replace")):
                    names.add(m[1] or m[2])
    return names


def align(other: list[Function], ours: list[Function]) -> tuple[list[tuple[Function, Function]], int]:
    """Pairs the two lists' functions. Returns the pairs and how many of them
    were paired by position between runs of equal sizes."""
    matcher = difflib.SequenceMatcher(None, [f.size for f in other], [f.size for f in ours], autojunk=False)
    pairs, by_position = [], 0
    a_end = b_end = 0
    for block in matcher.get_matching_blocks():  # ends with a zero-size block at both ends
        gap_a, gap_b = block.a - a_end, block.b - b_end
        if gap_a and gap_a == gap_b:
            pairs += [(other[a_end + k], ours[b_end + k]) for k in range(gap_a)]
            by_position += gap_a
        pairs += [(other[block.a + k], ours[block.b + k]) for k in range(block.size)]
        a_end, b_end = block.a + block.size, block.b + block.size
    return pairs, by_position


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("decomp_symbols", type=Path, help="the decomp's symbols.txt (the other release)")
    parser.add_argument("symbols", type=Path, help="this game's symbols.txt")
    parser.add_argument("--apply", action="store_true", help="rename generated names in place")
    parser.add_argument("--verbose", action="store_true", help="list every new name and every disagreement")
    args = parser.parse_args()

    _, other, _ = load(args.decomp_symbols)
    lines, ours, used = load(args.symbols)
    reserved = handwritten_names()
    pairs, by_position = align(other, ours)
    address_map = {a.address: b.address for a, b in pairs}

    def translate(name: str, a: Function, b: Function) -> str:
        m = ADDRESS_SUFFIX.match(name)
        if not m:
            return name
        value = int(m["hex"], 16)
        if value in address_map:
            return f"{m['base']}_{address_map[value]:08X}"
        return m["base"]

    renames: dict[int, str] = {}
    agree, differ, skipped = 0, [], []
    for a, b in pairs:
        if GENERATED.match(a.name) or not IDENTIFIER.match(a.name):
            continue
        if not GENERATED.match(b.name):
            if b.name == a.name:
                agree += 1
            else:
                differ.append((b, a.name))
            continue
        name = translate(a.name, a, b)
        if name in used or name in reserved:
            # Two decomp functions can share a name once an address is
            # dropped (DeadCounter_Set_ced48 and _806d0fb8): tell them apart
            # by this release's address.
            name = f"{name}_{b.address:08X}"
        if name in used or name in reserved:
            skipped.append((b, a.name))
            continue
        renames[b.line] = name
        used.add(name)

    print(f"{len(other)} and {len(ours)} functions; {len(pairs)} paired ({by_position} by position)")
    print(f"already named the same: {agree}; named differently (kept): {len(differ)}; "
          f"name in use (skipped): {len(skipped)}")
    print(f"new names: {len(renames)}")
    if args.verbose:
        for b, name in differ:
            print(f"  note: {b.address:08X} is named {b.name}, the decomp says {name}")
        for b, name in skipped:
            print(f"  skipped: {b.address:08X} {name}")
        for line, name in sorted(renames.items()):
            m = SYMBOL_LINE.match(lines[line])
            print(f"  {int(m['addr'], 16):08X} {m['name']} -> {name}")

    if args.apply and renames:
        for line, name in renames.items():
            m = SYMBOL_LINE.match(lines[line])
            rest = m["rest"]
            if "scope:" not in rest:
                rest += " scope:global"
            lines[line] = f"{name} = {m['sec']}:0x{m['addr']};{rest}"
        # newline="\n": keep LF line endings on Windows (decomp-toolkit writes LF).
        args.symbols.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        print(f"updated {args.symbols}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
