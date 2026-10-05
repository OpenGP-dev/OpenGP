#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that an installer package can build a game, without any game.

Compiles the test suite's recompiled fixture (tests/fixture, from a build
tree) and a small launcher the way the installer compiles a game (the flags,
libraries and compiler from the package's sdk/package.toml), links it, and
runs it with --version. CI uses it for the packages it cannot play.

    python tools/package_probe.py PACKAGE_DIR BUILD_DIR [--cxx COMPILER]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

PROBE_MAIN = r"""
#include "app/launcher.h"
#include "ppc_recomp_shared.h"

int main(int argc, char** argv) {
  app::Game game;
  game.id = "GTST01";
  game.title = "Package probe";
  game.module = &guest::g_module;
  return app::run(game, argc, argv);
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("--cxx")
    args = parser.parse_args()

    sdk = args.package / "sdk"
    config = tomllib.loads((sdk / "package.toml").read_text())
    build = config["build"]
    cxx = args.cxx
    if not cxx:
        toolchain = config.get("toolchain")
        if toolchain:
            cxx = str(args.package / "toolchain" / toolchain["dir"] / toolchain["cxx"])
        else:
            cxx = build["cxx"][0]
    generated = args.build / "tests" / "fixture" / "generated"
    sources = sorted(generated.glob("*.cpp"))
    if not sources:
        print(f"no generated fixture sources in {generated}")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        main_cpp = work / "probe_main.cpp"
        # The fixture's mid-function hooks are defined by the test suite; here they do nothing.
        header = (generated / "ppc_recomp_shared.h").read_text()
        hooks = "".join(f"PPC_HOOK({h}) {{}}\n" for h in re.findall(r"^PPC_HOOK\((\w+)\);", header, re.M))
        main_cpp.write_text(PROBE_MAIN + "namespace guest {\n" + hooks + "}\n")
        flags = build["cxx_flags"] + [f"-I{sdk / 'include'}", f"-I{generated}"]
        objects = []
        for i, src in enumerate([main_cpp] + sources):
            obj = work / f"{i}.o"
            subprocess.run([cxx, *flags, "-c", str(src), "-o", str(obj)], check=True)
            objects.append(str(obj))
        exe = work / ("probe" + build["executable_suffix"])
        libs = [str(sdk / lib) for lib in build["libs"]]
        subprocess.run([cxx, *objects, *build["link_flags"], "-Wl,--start-group", *libs, "-Wl,--end-group",
                        *build["system_libs"], "-o", str(exe)], check=True)
        out = subprocess.run([str(exe), "--version"], capture_output=True, text=True, check=True).stdout
        print(out.strip())
        if not out.startswith(f"OpenGP {config['version']}: Package probe"):
            print("unexpected --version output")
            return 1
    print("package probe: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
