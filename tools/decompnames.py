#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Name functions and data using a decompilation of another release of the game.

naari3/mkgp2-decomp (CC0) names Mario Kart Arcade GP 2's functions and
globals in the Japanese release, GNLJ82. Its code and data are the same as
GGPE02's apart from a handful of functions and objects of other sizes, which
shift everything after them. This lines up the two symbol lists by the sizes
of their functions (in address order) and of their objects (section by
section), and prints the decomp's names for this game's. With --apply it
renames symbols that still have generated names (fn_XXXXXXXX, dtor_XXXXXXXX,
lbl_XXXXXXXX):

  * runs of symbols whose sizes agree are paired in order; between two runs,
    the symbols left over are paired in order when both releases have the
    same number (the ones whose size differs);
  * where the decomp has split an object that decomp-toolkit found as one
    (to name a field of it, such as g_DebugPrintfEnable in the settings
    block), and its pieces cover exactly one of this game's objects, that
    object is split the same way;
  * a name is skipped if another symbol already has it, or if hand-written
    code (games/, runtime/) defines a function or hook of that name;
  * an address at the end of a decomp name (Stub_NoOp_80121c34) is the
    other release's: it becomes this release's address when it is a
    symbol's, and is dropped otherwise.

    python tools/decompnames.py gp2decomp/mkgp2-decomp/config/GNLJ82/symbols.txt \\
                                games/mkgp2/config/symbols.txt [--apply]

The decomp's names are its authors' reading of the code; the SDK names from
tools/sigmatch.py are kept where the two differ.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYMBOL_LINE = re.compile(r'^(?P<name>"[^"]*"|\S+) = (?P<sec>[^:;\s]+):0x(?P<addr>[0-9A-Fa-f]+);(?P<rest>.*)$')
SIZE = re.compile(r"size:0x([0-9A-Fa-f]+)")
DATA_KIND = re.compile(r"\bdata:\S+")
# Names decomp-toolkit (or Ghidra, in the decomp) makes up.
GENERATED = re.compile(r"^(fn|dtor|ctor|lbl|func|FUN|thunk_FUN|jumptable|DAT|PTR)_[0-9A-Fa-f]{8}$")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# A trailing address: 5 to 8 hex digits, at least one of them a digit (so
# words such as "_Added" are left alone).
ADDRESS_SUFFIX = re.compile(r"^(?P<base>\w+?)_(?P<hex>(?=[0-9A-Fa-f]*[0-9])[0-9A-Fa-f]{5,8})$")
HANDWRITTEN = re.compile(r'PPC_(?:FUNC|HOOK|EXTERN_FUNC)\((\w+)\)|^\s*function\s*=\s*"(\w+)"', re.M)


class Symbol:
    def __init__(self, line: int, name: str, section: str, address: int, size: int, rest: str):
        self.line, self.name, self.section, self.address, self.size, self.rest = line, name, section, address, size, rest


class SymbolList:
    def __init__(self, path: Path):
        self.lines = path.read_text(encoding="utf-8").splitlines()
        self.functions: list[Symbol] = []
        self.objects: dict[str, list[Symbol]] = defaultdict(list)  # by section
        self.names: set[str] = set()
        for i, line in enumerate(self.lines):
            m = SYMBOL_LINE.match(line)
            if not m:
                continue
            name = m["name"].strip('"')
            self.names.add(name)
            size = SIZE.search(m["rest"])
            if not size:
                continue
            sym = Symbol(i, name, m["sec"], int(m["addr"], 16), int(size[1], 16), m["rest"])
            if "type:function" in m["rest"]:
                self.functions.append(sym)
            elif "type:object" in m["rest"]:
                self.objects[sym.section].append(sym)
        self.functions.sort(key=lambda s: s.address)
        for objects in self.objects.values():
            objects.sort(key=lambda s: s.address)


def handwritten_names() -> set[str]:
    names = set()
    for top in ("games", "runtime"):
        for path in (ROOT / top).rglob("*"):
            if path.suffix in (".cpp", ".h", ".toml"):
                for m in HANDWRITTEN.finditer(path.read_text(encoding="utf-8", errors="replace")):
                    names.add(m[1] or m[2])
    return names


def align(other: list[Symbol], ours: list[Symbol]):
    """Pairs the two lists' symbols by size. Returns the pairs, how many of
    them were paired by position, and the gaps left: (other's, ours)."""
    matcher = difflib.SequenceMatcher(None, [s.size for s in other], [s.size for s in ours], autojunk=False)
    pairs, by_position, gaps = [], 0, []
    a_end = b_end = 0
    for block in matcher.get_matching_blocks():  # ends with a zero-size block at both ends
        gap_a, gap_b = block.a - a_end, block.b - b_end
        if gap_a and gap_a == gap_b:
            pairs += [(other[a_end + k], ours[b_end + k]) for k in range(gap_a)]
            by_position += gap_a
        elif gap_a or gap_b:
            gaps.append((other[a_end:block.a], ours[b_end:block.b]))
        pairs += [(other[block.a + k], ours[block.b + k]) for k in range(block.size)]
        a_end, b_end = block.a + block.size, block.b + block.size
    return pairs, by_position, gaps


def is_name(name: str) -> bool:
    return bool(IDENTIFIER.match(name)) and not GENERATED.match(name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("decomp_symbols", type=Path, help="the decomp's symbols.txt (the other release)")
    parser.add_argument("symbols", type=Path, help="this game's symbols.txt")
    parser.add_argument("--apply", action="store_true", help="rename generated names in place")
    parser.add_argument("--verbose", action="store_true", help="list every new name and every disagreement")
    args = parser.parse_args()

    other = SymbolList(args.decomp_symbols)
    ours = SymbolList(args.symbols)
    used = ours.names
    reserved = handwritten_names()

    function_pairs, function_by_position, _ = align(other.functions, ours.functions)
    object_pairs, object_by_position = [], 0
    splits: list[tuple[list[Symbol], Symbol]] = []  # the decomp's pieces, the object they cover
    for section, objects in other.objects.items():
        pairs, by_position, gaps = align(objects, ours.objects.get(section, []))
        object_pairs += pairs
        object_by_position += by_position
        for pieces, (target, *rest) in ((a, b) for a, b in gaps if len(a) > 1 and len(b) == 1):
            span = pieces[-1].address + pieces[-1].size - pieces[0].address
            if span == target.size and GENERATED.match(target.name) and any(is_name(p.name) for p in pieces):
                splits.append((pieces, target))

    address_map = {a.address: b.address for a, b in function_pairs + object_pairs}
    for pieces, target in splits:
        address_map.update({p.address: p.address - pieces[0].address + target.address for p in pieces})

    def translate(name: str) -> str:
        m = ADDRESS_SUFFIX.match(name)
        if not m:
            return name
        value = int(m["hex"], 16)
        return f"{m['base']}_{address_map[value]:08X}" if value in address_map else m["base"]

    def new_name(name: str, address: int) -> str | None:
        """The decomp's name for the symbol at `address` here, or None if it cannot be used."""
        name = translate(name)
        if name in used or name in reserved:
            # Two decomp symbols can share a name once an address is dropped
            # (DeadCounter_Set_ced48 and _806d0fb8): tell them apart by this
            # release's address.
            name = f"{name}_{address:08X}"
        if name in used or name in reserved:
            return None
        used.add(name)
        return name

    replacements: dict[int, list[str]] = {}  # line of ours -> its new lines
    counts = {"functions": [0, 0], "objects": [0, 0]}  # new names, already named the same
    differ, skipped = [], []

    def rename(kind: str, a: Symbol, b: Symbol) -> None:
        if not is_name(a.name):
            return
        if not GENERATED.match(b.name):
            if b.name in (translate(a.name), f"{translate(a.name)}_{b.address:08X}"):
                counts[kind][1] += 1
            else:
                differ.append((b, a.name))
            return
        name = new_name(a.name, b.address)
        if name is None:
            skipped.append((b, a.name))
            return
        rest = b.rest if "scope:" in b.rest else b.rest + " scope:global"
        replacements[b.line] = [f"{name} = {b.section}:0x{b.address:08X};{rest}"]
        counts[kind][0] += 1

    for a, b in function_pairs:
        rename("functions", a, b)
    for a, b in object_pairs:
        rename("objects", a, b)
    split_names = 0
    for pieces, target in splits:
        new_lines = []
        for p in pieces:
            address = p.address - pieces[0].address + target.address
            name = new_name(p.name, address) if is_name(p.name) else None
            kind = DATA_KIND.search(p.rest)
            attrs = f" // type:object size:0x{p.size:X}" + (f" {kind[0]}" if kind else "")
            if name:
                split_names += 1
                attrs += " scope:global"
            else:
                name = f"lbl_{address:08X}"
            new_lines.append(f"{name} = {target.section}:0x{address:08X};{attrs}")
        replacements[target.line] = new_lines

    print(f"functions: {len(other.functions)} and {len(ours.functions)}; {len(function_pairs)} paired "
          f"({function_by_position} by position); new names {counts['functions'][0]}, "
          f"already named the same {counts['functions'][1]}")
    print(f"objects: {sum(map(len, other.objects.values()))} and {sum(map(len, ours.objects.values()))}; "
          f"{len(object_pairs)} paired ({object_by_position} by position); new names {counts['objects'][0]}, "
          f"already named the same {counts['objects'][1]}; {len(splits)} objects split for {split_names} more")
    print(f"named differently (kept): {len(differ)}; name in use (skipped): {len(skipped)}")
    if args.verbose:
        for b, name in differ:
            print(f"  note: {b.address:08X} is named {b.name}, the decomp says {name}")
        for b, name in skipped:
            print(f"  skipped: {b.address:08X} {name}")
        for line, new_lines in sorted(replacements.items()):
            print(f"  {ours.lines[line].split(';')[0]} ->")
            for text in new_lines:
                print(f"      {text.split(';')[0]}")

    if args.apply and replacements:
        out = []
        for i, line in enumerate(ours.lines):
            out += replacements.get(i, [line])
        # newline="\n": keep LF line endings on Windows (decomp-toolkit writes LF).
        args.symbols.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
        print(f"updated {args.symbols}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
