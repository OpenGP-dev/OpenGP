# SPDX-License-Identifier: GPL-3.0-or-later
"""What the naming tools (sigmatch.py, decompnames.py, crossmatch.py) share.

Hand-written code refers to some guest functions by their symbols.txt names:
PPC_FUNC(fn_80289B54) overrides a function, and recomp.toml hooks name the
functions they call. Renaming such a symbol breaks the game's build, so the
tools neither rename one nor give its name to another symbol.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HANDWRITTEN = re.compile(r'PPC_(?:FUNC|HOOK|EXTERN_FUNC)\((\w+)\)|^\s*function\s*=\s*"(\w+)"', re.M)


def handwritten_names(root: Path = ROOT) -> set[str]:
    """The guest names that hand-written code in games/ and runtime/ uses."""
    names = set()
    for top in ("games", "runtime"):
        for path in (root / top).rglob("*"):
            if path.suffix in (".cpp", ".h", ".toml"):
                for m in HANDWRITTEN.finditer(path.read_text(encoding="utf-8", errors="replace")):
                    names.add(m[1] or m[2])
    return names
