#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put an OpenGP release's packages on the launcher's release.

The launcher updates itself and OpenGP from the latest release of
OpenGP-dev/opengp-launcher (LAUNCHER_RELEASES_URL). That release's tag is the
launcher's version and its files are OpenGP's packages, so a new OpenGP
release is served only once its packages replace the ones on the launcher's
release. This script does that: it takes the packages from an OpenGP release,
replaces the launcher release's with them, and gives the launcher release the
OpenGP release's name and notes.

    python tools/copy_release_to_launcher.py [TAG] [--launcher-ref REF] [--dry-run]

TAG is the OpenGP release to copy, v1.0.3 by default. REF is the launcher
release to put it on: LAUNCHER_REF in .github/workflows/package.yml by
default, which is the launcher the packages were built with and must be a
tag, not a branch. The launcher release's tag is its version, so it does not
change when only OpenGP does.

Needs the GitHub CLI (gh), signed in with write access to both repositories.
The OpenGP release must be published, as the launcher's release must be.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

OPENGP_REPO = "OpenGP-dev/OpenGP"
LAUNCHER_REPO = "OpenGP-dev/opengp-launcher"

# The packages, by the end of their name. Every release has all three.
SUFFIXES = ("windows-x64.zip", "linux-x64.tar.gz", "macos-arm64.tar.gz")
PACKAGE = re.compile(r"^OpenGP-(\d+\.\d+\.\d+)-(?:" + "|".join(re.escape(s) for s in SUFFIXES) + r")$")
LAUNCHER_REF = re.compile(r'^\s*LAUNCHER_REF:\s*(\S+)\s*$', re.MULTILINE)
ROOT = Path(__file__).resolve().parent.parent


def gh(*args: str, check: bool = True) -> str:
    """Run gh and return its output; raise with its error if it fails."""
    result = subprocess.run(["gh", *args], capture_output=True, text=True, encoding="utf-8")
    if check and result.returncode != 0:
        raise SystemExit(f"gh {' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout


def release(repo: str, ref: str) -> dict:
    out = gh("release", "view", ref, "--repo", repo,
             "--json", "tagName,name,isDraft,isPrerelease,url,body,assets")
    return json.loads(out)


def launcher_ref() -> str:
    """LAUNCHER_REF from the package workflow: the launcher release."""
    text = (ROOT / ".github" / "workflows" / "package.yml").read_text(encoding="utf-8")
    match = LAUNCHER_REF.search(text)
    if not match:
        raise SystemExit("LAUNCHER_REF is not in .github/workflows/package.yml")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tag", nargs="?", default="latest",
                        help="the OpenGP release to copy (default: latest)")
    parser.add_argument("--launcher-ref", help="the launcher release to put it on "
                        "(default: LAUNCHER_REF in the package workflow)")
    parser.add_argument("--dry-run", action="store_true", help="say what would change, change nothing")
    args = parser.parse_args()

    source = release(OPENGP_REPO, args.tag)
    if source["isDraft"] or source["isPrerelease"]:
        raise SystemExit(f"{source['tagName']} is a draft or prerelease; publish it first")
    version = source["tagName"].removeprefix("v")
    packages = {a["name"]: a for a in source["assets"] if PACKAGE.match(a["name"])}
    if len(packages) != len(SUFFIXES):
        raise SystemExit(f"{source['tagName']} has {len(packages)} of the {len(SUFFIXES)} packages "
                         f"({', '.join(sorted(packages)) or 'none'})")
    for name, asset in packages.items():
        if PACKAGE.match(name).group(1) != version:
            raise SystemExit(f"{name} is not OpenGP {version}, the version of {source['tagName']}")
        if not asset.get("digest", "").startswith("sha256:"):
            raise SystemExit(f"{name} has no SHA-256 digest; the launcher will not offer it")
    if "SHA256SUMS" not in {a["name"] for a in source["assets"]}:
        raise SystemExit(f"{source['tagName']} has no SHA256SUMS")

    ref = args.launcher_ref or launcher_ref()
    target = release(LAUNCHER_REPO, ref)
    if target["isDraft"] or target["isPrerelease"]:
        raise SystemExit(f"the launcher's {ref} is a draft or prerelease; publish it first")
    if target["tagName"] != ref:
        raise SystemExit(f"the launcher's {ref} is tagged {target['tagName']}")

    print(f"{source['tagName']} ({', '.join(sorted(packages))})")
    print(f"  -> {LAUNCHER_REPO} {target['tagName']} ({target['url']})")
    stale = [a["name"] for a in target["assets"]
             if a["name"] == "SHA256SUMS" or a["name"].startswith("OpenGP-")]
    for name in stale:
        print(f"  remove {name}")
    for name in sorted(packages) + ["SHA256SUMS"]:
        print(f"  add    {name}")
    if target["name"] != source["name"]:
        print(f"  rename {target['name']!r} -> {source['name']!r}")
    print("  notes: the OpenGP release's")
    if args.dry_run:
        print("Dry run: nothing changed.")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        gh("release", "download", source["tagName"], "--repo", OPENGP_REPO,
           "--pattern", "OpenGP-*", "--pattern", "SHA256SUMS", "--dir", tmp, "--clobber")
        for name in stale:
            gh("release", "delete-asset", target["tagName"], name, "--repo", LAUNCHER_REPO, "--yes")
        files = [str(Path(tmp) / name) for name in sorted(packages) + ["SHA256SUMS"]]
        gh("release", "upload", target["tagName"], *files, "--repo", LAUNCHER_REPO, "--clobber")
        notes = Path(tmp) / "notes.md"
        notes.write_text(source["body"], encoding="utf-8")
        gh("release", "edit", target["tagName"], "--repo", LAUNCHER_REPO,
           "--title", source["name"], "--notes-file", str(notes))

    after = release(LAUNCHER_REPO, ref)
    names = {a["name"] for a in after["assets"]}
    missing = ({a["name"] for a in source["assets"]} - names)
    if missing:
        raise SystemExit(f"the launcher's {ref} is missing {', '.join(sorted(missing))} after the copy")
    print(f"Done: the launcher now serves {source['name']} from {after['url']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
