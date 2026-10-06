# The installer package

OpenGP is distributed as an **installer package**: everything except the games. On the player's
machine, the installer builds each game from the player's own disc image, so no code or data from
the games is ever distributed (see [game-files.md](game-files.md) for why).

## Using it

1. Unpack `OpenGP-<version>-windows-x64.zip`, `OpenGP-<version>-linux-x64.tar.gz` or
   `OpenGP-<version>-macos-arm64.tar.gz` anywhere.
2. Put your images in its `ISO` folder (or anywhere, and give their paths to the installer).
3. Run `OpenGP` (on Windows, double-click `OpenGP.exe`, or drop an image on it).

For each game it finds an image of, the installer:

1. checks the image: the game ID, and the SHA-1 of its `main.dol` against the supported release;
2. recompiles `main.dol` into C++ with the same recompiler the source build uses;
3. compiles that and the game's hand-written code, and links it with the prebuilt runtime into
   `opengp-mkgp2` / `opengp-mkgp1` next to it;
4. remembers the image's path in `user/images.toml`, so the game finds it later.

This takes one to three minutes per game, once. Either game can be installed on its own: a
missing image only skips that game. Running the installer again rebuilds only games that an
earlier version of the package built (`--force` rebuilds them anyway), so after updating the
package, run it again.

```
OpenGP [IMAGE...] [--force] [--jobs N] [--cxx COMPILER] [--keep-build]
```

- `--jobs N`: compile N files at a time (default: one per CPU thread).
- `--cxx COMPILER`: use this C++ compiler instead of the package's.
- `--keep-build`: keep the generated C++ and objects in `build/<game ID>/` (by default they are
  deleted once the game is built).

The games then run like a source build's (see [playing.md](playing.md)), with their settings,
saves and screenshots in the package's `user/` folder.

## The compiler

- **Windows**: the installer downloads [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (Clang
  for Windows, free to redistribute) on first use, about 190 MB, checks its SHA-256 and unpacks
  the x86-64 part (about 480 MB) into `toolchain/`. It uses the copies of `curl` and `tar` that
  come with Windows 10 and later.
- **Linux**: the system's C++ compiler (`c++`, `g++` or `clang++`): GCC 12 or later, or Clang 16
  or later (for example `sudo apt install g++`).
- **macOS** (Apple Silicon, macOS 13 or later): Apple's Clang from the Xcode Command Line Tools
  (`xcode-select --install`).

### macOS: the first start

The package is not signed by Apple (that needs a paid developer account), so macOS refuses to
open the downloaded `OpenGP` until you allow it once: right-click it in Finder and choose
**Open**, or run `xattr -dr com.apple.quarantine <package folder>` in Terminal. The games the
installer builds on your Mac are yours and open normally.

The runtime in the package is built with the same compiler and flags that the installer uses, so
the two link together.

## What is in it

```
OpenGP[.exe]            the installer
sdk/package.toml        the version, the pinned compiler, compile and link settings
sdk/include/            the runtime's headers
sdk/lib/                the runtime as static libraries (with SDL3 and Dear ImGui)
sdk/games/<game>/       game.toml, recomp.toml, config/symbols.txt, patches.cpp
sdk/games/common/       the launcher's main() and the code the games share
ISO/                    for your images
docs/, README.md, LICENSE, CHANGELOG.md
```

After installing: `toolchain/` (Windows), `opengp-<game>[.exe]`, `user/`.

## Building the package

The package is a CMake install of a build with `OPENGP_PACKAGE=ON` (see `cmake/package.cmake`).
CI builds both (`.github/workflows/package.yml`), runs the tests, checks each package with
`tools/package_probe.py` (which builds the test suite's recompiled fixture the way the installer
builds a game), and attaches them to releases.

**Windows**, with the llvm-mingw release pinned in `cmake/package.cmake` unpacked somewhere:

```sh
export OPENGP_LLVM_MINGW=/path/to/llvm-mingw-20260922-ucrt-x86_64   # PowerShell: $env:OPENGP_LLVM_MINGW = ...
cmake --preset package-windows
cmake --build --preset package-windows
cmake --install build/package-windows        # -> out/OpenGP-windows-x64
```

**macOS** (Apple Silicon), with Apple's Clang:

```sh
cmake --preset package-macos
cmake --build --preset package-macos
cmake --install build/package-macos          # -> out/OpenGP-macos-arm64
```

**Linux**, with GCC 12 (built on the oldest distribution to support, since the games are linked
against the player's system libraries):

```sh
CC=gcc-12 CXX=g++-12 cmake --preset package-linux
cmake --build --preset package-linux
cmake --install build/package-linux          # -> out/OpenGP-linux-x64
```

A package build can be tried out directly: run `out/OpenGP-windows-x64/OpenGP.exe` with an image.

Things to keep in step:
- The llvm-mingw version and SHA-256 in `cmake/package.cmake` are both what the package's runtime
  is built with and what its installer downloads.
- `cxx_flags` in `cmake/package.cmake` must match the flags the generated code needs
  (`opengp_guest_flags` in `CMakeLists.txt`).
- The link libraries in `cmake/package.cmake` must cover what the runtime and SDL3 need; the
  probe catches a missing one.
