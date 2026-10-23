#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the notices of the third-party code in the Qt the packages carry.

An installer package's launcher comes with Qt's libraries and plugins, and Qt
builds third-party code into them (PCRE2, HarfBuzz, libjpeg-turbo and more)
whose licences want their notices to come with a binary copy. Qt lists that
code in the qt_attribution.json files of its source. This writes the entries
of the Qt modules the launcher deploys as one text file, each with its
copyright and its licence in full, into thirdparty/qt/third-party.txt, which
cmake/package.cmake installs as licenses/Qt-third-party.txt. Run it again
whenever the packages' Qt (QT_VERSION in .github/workflows/package.yml)
changes.

    python tools/qt_notices.py VERSION MODULE_DIR...

Each MODULE_DIR is a checkout of a Qt module at the tag v<VERSION>: qtbase,
qtdeclarative and qtsvg.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import textwrap
from pathlib import Path

# Qt modules the package doesn't carry: Qt SQL, Qt Test and Qt Wayland (the
# launcher uses XWayland on a Wayland desktop).
SKIPPED_MODULES = {"qtsql", "qttestlib", "qttestinternals_private", "qtwaylandcompositor"}
# Code only for systems the packages aren't for.
SKIPPED_FOLDERS = {"android", "wasm"}

MODULE_NAMES = {
    "qtcore": "Qt Core",
    "qtgui": "Qt Gui",
    "qtnetwork": "Qt Network",
    "qtdbus": "Qt D-Bus",
    "qtqml": "Qt Qml",
    "qtquick": "Qt Quick",
    "qtquickcontrols": "Qt Quick Controls",
    "qtsvg": "Qt SVG",
}

# The file's opening paragraphs, wrapped once the version and modules are in.
HEADER = [
    "Third-party code in Qt {version}",
    "An OpenGP installer package carries Qt {version} for its launcher, OpenGP: Qt Core, Qt Gui, "
    "Qt Network, Qt Qml, Qt Quick, Qt Quick Controls and Qt SVG, with the libraries and plugins they "
    "need. Qt is under the LGPL-3.0 (Qt-LGPL-3.0.txt). Qt builds the third-party code below into "
    "those libraries and plugins. Each keeps its own licence, given here in full with its copyright "
    "notices.",
    "This list is made by tools/qt_notices.py, in OpenGP's source, from the qt_attribution.json "
    "files in the source of these Qt {version} modules: {modules}. It leaves out what is only in "
    "Qt's tools, examples and tests, in Qt modules the package doesn't carry, or in Qt for Android "
    "and WebAssembly. Some of the rest is only in Qt for one system, or only when Qt is built with "
    "its own copy of the code rather than the system's.",
]

RULE = "-" * 78
OUTPUT = Path(__file__).resolve().parents[1] / "thirdparty" / "qt" / "third-party.txt"


def text_of(path: Path) -> str:
    text = path.read_bytes().decode("utf-8")
    lines = [line.rstrip() for line in text.replace("\r\n", "\n").split("\n")]
    return "\n".join(lines).strip("\n")


def licence_files(entry: dict, folder: Path, module: Path) -> list[Path]:
    names = entry.get("LicenseFiles") or ([entry["LicenseFile"]] if entry.get("LicenseFile") else [])
    if names:
        return [folder / name for name in names]
    # No file of its own: the module's copy of each licence its SPDX expression names.
    ids = [i for i in re.findall(r"[\w.+-]+", entry["LicenseId"]) if i not in ("AND", "OR", "WITH")]
    return [module / "LICENSES" / f"{i}.txt" for i in ids]


def entries(module: Path):
    for path in sorted(module.rglob("qt_attribution.json")):
        folder = path.parent
        if SKIPPED_FOLDERS & set(folder.relative_to(module).parts):
            continue
        data = json.loads(path.read_text(encoding="utf-8"), strict=False)
        for entry in data if isinstance(data, list) else [data]:
            if "libs" not in entry.get("QtParts", ["libs"]) or entry["QDocModule"] in SKIPPED_MODULES:
                continue
            yield entry, folder


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("modules", type=Path, nargs="+")
    args = parser.parse_args()

    found = []
    for module in args.modules:
        for entry, folder in entries(module):
            found.append((entry["Name"].lower(), entry, folder, module))
    found.sort(key=lambda item: item[0])

    names = [module.resolve().name for module in args.modules]
    modules = names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]
    out = []
    for paragraph in HEADER:
        paragraph = paragraph.format(version=args.version, modules=modules)
        out += [textwrap.fill(paragraph, 78, break_on_hyphens=False), ""]
    seen: dict[str, str] = {}
    for _, entry, folder, module in found:
        title = entry["Name"]
        module_name = MODULE_NAMES.get(entry["QDocModule"], entry["QDocModule"])
        out += [RULE, f"{title} ({module_name})", ""]
        if entry.get("Version"):
            out.append(f"Version: {entry['Version']}")
        if entry.get("Homepage"):
            out.append(entry["Homepage"])
        out += [entry["QtUsage"], ""]
        if entry.get("CopyrightFile"):
            out.append(text_of(folder / entry["CopyrightFile"]))
        else:
            copyright = entry["Copyright"]
            out.append("\n".join(copyright) if isinstance(copyright, list) else copyright)
        out += ["", f"Licence: {entry['License']} ({entry['LicenseId']})"]
        for path in licence_files(entry, folder, module):
            text = text_of(path)
            out.append("")
            if text in seen:
                out.append(f"(The same text as for {seen[text]} above.)")
            else:
                seen[text] = title
                out.append(text)
        out.append("")
    text = "\n".join(line.rstrip() for line in "\n".join(out).split("\n"))
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(found)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
