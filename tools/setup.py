#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the game files OpenGP builds from.

For each disc image (default: every *.iso / *.gcm in ISO/ and private/), this
reads the game ID, extracts sys/main.dol into orig/<ID>/sys/main.dol, and
checks its SHA-1 against the supported revisions. An image whose header has a
placeholder ID (some converted dumps say "RELSAB") is identified by its
main.dol. Only a main.dol of a supported revision is written. Then it
downloads the pinned decomp-toolkit into build/tools/, for regenerating the
symbol lists (skip that with --no-dtk).

    python3 tools/setup.py [IMAGE ...] [--no-dtk]

Nothing here is ever committed: ISO/, private/, orig/ and build/ are gitignored.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import ssl
import stat
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DTK_VERSION = "v1.8.4"
# SHA-256 of each decomp-toolkit DTK_VERSION release asset (from the release's digests).
DTK_SHA256 = {
    "dtk-linux-aarch64": "889a551d57916772f21a29c997579e0508f268c96bcbc946a0a66bff029cf97c",
    "dtk-linux-x86_64": "7c86b1994401bde0cad1d308e6c04cccb65e85453ccc5062ac12baab3b03d320",
    "dtk-macos-arm64": "5ba1fd76adf937847277a1ebe2238b698df0020c3dbbde23ecb126f00fb10353",
    "dtk-macos-x86_64": "84bcab745a070bb970cf4bc0d69ddce9f6a58d5de257999ccc8adfa1567d17c9",
    "dtk-windows-arm64.exe": "6d2567f6017f6add6e493f86feb3c4fd7064a17ca7f972f36f29aa3af3488871",
    "dtk-windows-x86_64.exe": "6a693e95aa4d30acccc673343302a4abdfa3146bd0fb15cb1a2bce924cbc782a",
}

# Supported releases: game ID -> (name, main.dol SHA-1).
GAMES = {
    "GGPE01": ("Mario Kart Arcade GP (USA)", "0e84df7cac27eab7d6dbb1892933ae3bf762f742"),
    "GGPE02": ("Mario Kart Arcade GP 2 (USA)", "2969c177219dda71ad36b41575bede7ad4637f96"),
}

GC_MAGIC = 0xC2339F3D
MAX_DOL = 16 << 20  # the games' main.dol is a few MiB; the GameCube has 24 MiB of RAM


class BadImage(Exception):
    """An image that cannot be read as a disc."""


def download(url: str, dest: Path) -> None:
    """Downloads `url` to `dest` (through `dest`.part, so a failed download
    leaves nothing behind). Python from python.org on macOS comes without CA
    certificates (until its "Install Certificates" script is run), so the
    system's are loaded too."""
    context = ssl.create_default_context()
    if sys.platform == "darwin" and os.path.exists("/etc/ssl/cert.pem"):
        context.load_verify_locations("/etc/ssl/cert.pem")
    part = dest.with_name(dest.name + ".part")
    try:
        with urllib.request.urlopen(url, context=context, timeout=60) as response, open(part, "wb") as out:
            shutil.copyfileobj(response, out)
        part.replace(dest)
    finally:
        part.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def dtk_path() -> Path:
    exe = "dtk.exe" if os.name == "nt" else "dtk"
    return ROOT / "build" / "tools" / exe


def dtk_asset() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "linux":  # the Linux builds are named aarch64, the others arm64
        arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(machine, machine)
        return f"dtk-linux-{arch}"
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(machine, machine)
    if system == "windows":
        return f"dtk-windows-{arch}.exe"
    if system == "darwin":
        return f"dtk-macos-{arch}"
    return f"dtk-{system}-{arch}"


def ensure_dtk() -> bool:
    """Downloads decomp-toolkit DTK_VERSION unless it is already there (a
    stamp file next to it names the version). False, with a message, if it
    cannot."""
    path = dtk_path()
    stamp = path.with_name(path.name + ".version")
    asset = dtk_asset()
    expected = DTK_SHA256.get(asset)
    try:
        if path.exists() and stamp.exists() and stamp.read_text().strip() == f"{DTK_VERSION} {expected}":
            return True
    except OSError:
        pass
    if expected is None:
        print(f"decomp-toolkit has no {asset} build pinned here; get dtk {DTK_VERSION} yourself into {path} "
              "if you need to regenerate the symbol lists")
        return False
    url = f"https://github.com/encounter/decomp-toolkit/releases/download/{DTK_VERSION}/{asset}"
    print(f"Downloading decomp-toolkit {DTK_VERSION} from {url}")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".download")
        download(url, tmp)
        actual = sha256(tmp)
        if actual != expected:
            tmp.unlink(missing_ok=True)
            print(f"decomp-toolkit download has SHA-256 {actual}, expected {expected}: not used")
            return False
        tmp.replace(path)
        if os.name != "nt":
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        stamp.write_text(f"{DTK_VERSION} {expected}\n")
        return True
    except (urllib.error.URLError, OSError) as e:
        print(f"Could not download decomp-toolkit ({e}). It is only needed to regenerate the symbol lists; "
              "the games build without it.")
        return False


def read_game_id(image: Path) -> str | None:
    with image.open("rb") as f:
        header = f.read(0x20)
    if len(header) < 0x20 or struct.unpack(">I", header[0x1C:0x20])[0] != GC_MAGIC:
        return None
    return header[:6].decode("ascii", errors="replace")


def read_dol(image: Path) -> bytes:
    """The executable (sys/main.dol): its offset is in the disc header, its size
    follows from its section table. Checked against the image's size."""
    size = image.stat().st_size
    with image.open("rb") as f:
        f.seek(0x420)
        raw = f.read(4)
        if len(raw) < 4:
            raise BadImage("truncated: no disc header")
        dol = struct.unpack(">I", raw)[0]
        if dol + 0x100 > size:
            raise BadImage("damaged: the executable's offset is past the end of the image")
        f.seek(dol)
        header = f.read(0x100)
        offsets = struct.unpack(">18I", header[0:72])
        sizes = struct.unpack(">18I", header[144:216])
        end = max([0x100] + [o + s for o, s in zip(offsets, sizes) if s])
        if end > MAX_DOL or dol + end > size:
            raise BadImage("truncated or damaged: the executable's sections run past the end of the image")
        f.seek(dol)
        data = f.read(end)
    if len(data) != end:
        raise BadImage("truncated: the executable is cut short")
    return data


def find_images() -> list[Path]:
    images: list[Path] = []
    for folder in ("ISO", "private"):
        d = ROOT / folder
        if d.is_dir():
            images += sorted(p for p in d.rglob("*") if p.suffix.lower() in (".iso", ".gcm"))
    return images


def write_dol(game_id: str, dol: bytes) -> str:
    """orig/<ID>/sys/main.dol, through a .part file; left as it is if it
    already holds this executable."""
    out = ROOT / "orig" / game_id / "sys" / "main.dol"
    if out.is_file() and out.read_bytes() == dol:
        return "already there"
    out.parent.mkdir(parents=True, exist_ok=True)
    part = out.with_name(out.name + ".part")
    part.write_bytes(dol)
    part.replace(out)
    return "written"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="*", type=Path, help="disc images (default: those in ISO/ and private/)")
    parser.add_argument("--no-dtk", action="store_true", help="don't download decomp-toolkit")
    args = parser.parse_args(argv)

    named = bool(args.images)
    images = [p.resolve() for p in args.images] or find_images()
    if not images:
        print("No disc images found. Put them in ISO/ or private/, or pass paths. See docs/game-files.md.")
        return 1
    extracted, problems = 0, 0
    for image in images:
        try:
            game_id = read_game_id(image)
            if game_id is None:
                print(f"{image.name}: not a GameCube/Triforce disc image, skipped")
                problems += named
                continue
            dol = read_dol(image)
        except (OSError, BadImage, struct.error) as e:
            print(f"{image.name}: cannot be read ({e}), skipped")
            problems += 1
            continue
        actual = hashlib.sha1(dol).hexdigest()
        name, expected = GAMES.get(game_id, (None, None))
        if name is None:
            # Some converted dumps have a placeholder header ("RELSAB"): go by
            # the executable instead.
            found = [gid for gid, (_, sha) in GAMES.items() if sha == actual]
            if not found:
                print(f"{image.name}: {game_id} is not a supported game, skipped")
                problems += named
                continue
            print(f"{image.name}: its header says {game_id}, but it holds {found[0]}'s main.dol")
            game_id = found[0]
            name, expected = GAMES[game_id]
        if actual != expected:
            problems += 1
            print(f"{name} [{game_id}]: main.dol SHA-1 {actual} does not match the supported revision "
                  f"({expected}). This revision is not supported yet; orig/{game_id}/ is left as it is.")
            continue
        try:
            how = write_dol(game_id, dol)
        except OSError as e:
            print(f"{name} [{game_id}]: cannot write orig/{game_id}/sys/main.dol ({e})")
            problems += 1
            continue
        print(f"{name} [{game_id}]: orig/{game_id}/sys/main.dol OK ({how})")
        extracted += 1
    if not args.no_dtk:
        ensure_dtk()  # for regenerating the symbol lists (docs/game-files.md)
    if extracted == 0:
        print("No supported game was extracted. See docs/game-files.md.")
        return 1
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
