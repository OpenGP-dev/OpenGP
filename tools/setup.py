#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the game files OpenGP builds from.

For each disc image (default: every *.iso / *.gcm in ISO/ and private/), this
reads the game ID, extracts sys/main.dol into orig/<ID>/sys/main.dol, and
checks its SHA-1 against the supported revisions. An image whose header has a
placeholder ID (some converted dumps say "RELSAB") is identified by its
main.dol. It also downloads the pinned decomp-toolkit into build/tools/, for
regenerating the symbol lists.

    python tools/setup.py [IMAGE ...]

Nothing here is ever committed: ISO/, private/, orig/ and build/ are gitignored.
"""

from __future__ import annotations

import hashlib
import os
import platform
import shutil
import ssl
import stat
import struct
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DTK_VERSION = "v1.8.4"

# Supported releases: game ID -> (name, main.dol SHA-1).
GAMES = {
    "GGPE01": ("Mario Kart Arcade GP (USA)", "0e84df7cac27eab7d6dbb1892933ae3bf762f742"),
    "GGPE02": ("Mario Kart Arcade GP 2 (USA)", "2969c177219dda71ad36b41575bede7ad4637f96"),
}

GC_MAGIC = 0xC2339F3D


def download(url: str, dest) -> None:
    """Downloads `url` to `dest`. Python from python.org on macOS comes without
    CA certificates (until its "Install Certificates" script is run), so the
    system's are loaded too."""
    context = ssl.create_default_context()
    if sys.platform == "darwin" and os.path.exists("/etc/ssl/cert.pem"):
        context.load_verify_locations("/etc/ssl/cert.pem")
    with urllib.request.urlopen(url, context=context) as response, open(dest, "wb") as out:
        shutil.copyfileobj(response, out)


def dtk_path() -> Path:
    exe = "dtk.exe" if os.name == "nt" else "dtk"
    return ROOT / "build" / "tools" / exe


def dtk_asset() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    if system == "windows":
        return f"dtk-windows-{arch}.exe"
    if system == "darwin":
        return f"dtk-macos-{arch}"
    return f"dtk-linux-{arch}"


def ensure_dtk() -> Path:
    path = dtk_path()
    if path.exists():
        return path
    url = f"https://github.com/encounter/decomp-toolkit/releases/download/{DTK_VERSION}/{dtk_asset()}"
    print(f"Downloading decomp-toolkit {DTK_VERSION} from {url}")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".part")
    download(url, tmp)
    tmp.replace(path)
    if os.name != "nt":
        path.chmod(path.stat().st_mode | stat.S_IEXEC)
    return path


def read_game_id(image: Path) -> str | None:
    with image.open("rb") as f:
        header = f.read(0x20)
    if len(header) < 0x20 or struct.unpack(">I", header[0x1C:0x20])[0] != GC_MAGIC:
        return None
    return header[:6].decode("ascii", errors="replace")


def read_dol(image: Path) -> bytes:
    """The executable (sys/main.dol): its offset is in the disc header, its size
    follows from its section table."""
    with image.open("rb") as f:
        f.seek(0x420)
        dol = struct.unpack(">I", f.read(4))[0]
        f.seek(dol)
        header = f.read(0x100)
        offsets = struct.unpack(">18I", header[0:72])
        sizes = struct.unpack(">18I", header[144:216])
        end = max([0x100] + [o + s for o, s in zip(offsets, sizes) if s])
        f.seek(dol)
        return f.read(end)



def find_images() -> list[Path]:
    images: list[Path] = []
    for folder in ("ISO", "private"):
        d = ROOT / folder
        if d.is_dir():
            images += sorted(p for p in d.rglob("*") if p.suffix.lower() in (".iso", ".gcm"))
    return images


def main(argv: list[str]) -> int:
    images = [Path(a).resolve() for a in argv] or find_images()
    if not images:
        print("No disc images found. Put them in ISO/ or private/, or pass paths. See docs/game-files.md.")
        return 1
    ensure_dtk()  # for regenerating the symbol lists (docs/game-files.md)
    ok = True
    for image in images:
        game_id = read_game_id(image)
        if game_id is None:
            print(f"{image.name}: not a GameCube/Triforce disc image, skipped")
            continue
        dol = read_dol(image)
        actual = hashlib.sha1(dol).hexdigest()
        name, expected = GAMES.get(game_id, (None, None))
        if name is None:
            # Some converted dumps have a placeholder header ("RELSAB"): go by
            # the executable instead.
            found = [gid for gid, (_, sha) in GAMES.items() if sha == actual]
            if not found:
                print(f"{image.name}: {game_id} is not a supported game, skipped")
                continue
            print(f"{image.name}: its header says {game_id}, but it holds {found[0]}'s main.dol")
            game_id = found[0]
            name, expected = GAMES[game_id]
        out = ROOT / "orig" / game_id / "sys" / "main.dol"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(dol)
        if actual == expected:
            print(f"{name} [{game_id}]: orig/{game_id}/sys/main.dol OK")
        else:
            ok = False
            print(f"{name} [{game_id}]: main.dol SHA-1 {actual} does not match the supported revision "
                  f"({expected}). This revision is not supported yet.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
