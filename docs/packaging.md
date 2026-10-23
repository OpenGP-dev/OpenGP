# The installer package

OpenGP releases are installer packages. A package has everything except the games. Its launcher,
`OpenGP`, builds each game on the player's computer from the player's own disc image, so no code
or data from the games is ever distributed. [game-files.md](game-files.md) explains why that
matters.

## Requirements

- **Windows**: Windows 10 version 1809 or later, 64-bit, with a graphics driver that supports
  Vulkan. Most GPUs have one; install your GPU maker's current driver if the game says it found
  none.
- **Linux**: x86-64, a Vulkan driver (Mesa's, or the GPU maker's), and GCC 12 or later or Clang 16
  or later to build the games. OpenGP runs on Ubuntu 22.04, Debian 12 or newer, with an X11
  desktop, or XWayland on a Wayland one.
- **macOS**: macOS 13 or later on Apple Silicon, with the Xcode Command Line Tools.

[The compiler](#the-compiler) says where the compiler that builds the games comes from.

The package's folder must be one you can write to: the games, their compiler and your saves all go
in it. Your Documents folder or your desktop is fine; Program Files is not.

On Linux, OpenGP's window uses Qt's X11 support. The package carries the libraries for it that a
desktop may lack: `libxcb-cursor0`, which the Ubuntu and Xubuntu desktops of 22.04 and 24.04 leave
out, and the other xcb-util libraries. The rest come with an X11 desktop; Ubuntu's, Xubuntu's and
Kubuntu's have them all. If OpenGP doesn't open, run it in a terminal. If it says that it could not
load the Qt platform plugin "xcb", install them. On Debian and Ubuntu:

```sh
sudo apt install libxkbcommon-x11-0 libxcb-xkb1 libxcb-randr0 libxcb-render0 libxcb-shape0 \
  libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-glx0 libx11-xcb1 libsm6 libice6 \
  libegl1 libgl1 libopengl0 libfontconfig1
```

## Using it

1. Unpack `OpenGP-<version>-windows-x64.zip`, `OpenGP-<version>-linux-x64.tar.gz` or
   `OpenGP-<version>-macos-arm64.tar.gz` into a folder you can write to. Unpack the whole archive:
   on Windows, right-click the zip and choose **Extract All**. OpenGP cannot run from inside the
   zip.
2. Open `OpenGP`, the launcher: `OpenGP.exe` on Windows, `OpenGP.app` on macOS. The first time,
   Windows and macOS may stop you; see [Windows](#opening-the-package-on-windows-the-first-time)
   and [macOS](#opening-the-package-on-macos-the-first-time) below.
3. Add your disc image of each game with **Add disc image**, or drop it on the window. Images in
   the package's `ISO` folder are found without that.
4. Choose **Install**. OpenGP builds the game from your image, once. Then **Play** starts it.

To build a game, OpenGP's installer does four things:

1. It checks the image's game ID, and the SHA-1 of its `main.dol` against the supported release.
2. It recompiles `main.dol` into C++ with the same recompiler the source build uses.
3. It compiles that C++ and the game's hand-written code, and links them with the prebuilt runtime
   into `opengp-mkgp2` or `opengp-mkgp1` in the package folder.
4. It records the image's path in `user/images.toml`, so the game finds the image later.

Building a game takes one to three minutes, once. You can install either game on its own. After
updating the package, OpenGP shows **Update** on each game that an earlier version built, and
builds it again. If you move an image, add it again in OpenGP, and the game uses it from then on.

OpenGP starts the games, which run like a source build's games, as [playing.md](playing.md)
describes. Their settings, saves and screenshots go in the package's `user/` folder.

### Updating

Your saves, Mario Cards and settings are in the package's `user/` folder. To update OpenGP:

1. Unpack the new package over the old one, replacing its files. Or unpack it into a new folder and
   move `user/` into it from the old one. Move your images from the old `ISO/` folder into the new
   one too, and `private/` if you made one. On Windows, move `toolchain/` as well, so the compiler
   isn't downloaded again if the new version uses the same one. Images you keep elsewhere stay
   where they are.
2. Open `OpenGP` and choose **Update** on each game. OpenGP builds it again for the new version,
   from the image it recorded or one in `ISO/`. If you unpacked into a new folder, don't delete the
   old one until the games start from the new one.

Update every linked cabinet: cabinets on different versions of OpenGP may not link.

### On the command line

`opengp-install` (`opengp-install.exe`), next to OpenGP, is the same installer for the command
line. It builds each game from the images you give it, or without any, from the images the games
were built from before, then those in `ISO/`. On Windows you can also drop an image on
`opengp-install.exe`. If it finds no image for a game, it skips that game and builds the other.

When you run it again, it rebuilds only the games that an earlier version of the package built.
`--force` rebuilds every game. If you move an image, give `opengp-install` its new path (or drop it
on `opengp-install.exe`), and the game uses it from then on.

```
opengp-install [IMAGE...] [--force] [--jobs N] [--cxx COMPILER] [--keep-build]
```

- `--jobs N` compiles N files at a time. The default is one per CPU thread.
- `--cxx COMPILER` uses this C++ compiler instead of the package's.
- `--keep-build` keeps the generated C++ and the object files in `build/<game ID>/`. The installer
  deletes them once the game is built otherwise.

## The compiler

**Windows.** On first use the installer downloads [llvm-mingw](https://github.com/mstorsjo/llvm-mingw),
a build of Clang for Windows that anyone may redistribute. The download is about 190 MB. The
installer checks its SHA-256 and unpacks the x86-64 part, about 480 MB, into `toolchain/`. It uses
the `curl` and `tar` that come with Windows 10 and later.

**Linux.** The installer uses the system's C++ compiler: the first of `c++`, `g++`, `clang++` and
the versioned names (`g++-12` to `g++-20`, `clang++-16` to `clang++-25`) that is GCC 12 or later or
Clang 16 or later. It skips older ones. On Ubuntu 22.04, whose `g++` is GCC 11,
`sudo apt install g++-12` installs one; on newer Debian and Ubuntu, `sudo apt install g++` does.

**macOS.** The installer uses Apple's Clang from the Xcode Command Line Tools, which
`xcode-select --install` installs. The package needs Apple Silicon and macOS 13 or later.

CI builds the runtime in each package with the flags the installer uses. On Windows and macOS it
uses the same compiler too. On Linux it uses GCC 12, the oldest compiler the installer accepts, so
the runtime links with any compiler the installer finds.

### Opening the package on Windows the first time

The package isn't signed, because signing costs money every year. Windows may warn that it
"protected your PC" the first time you open `OpenGP.exe`. Choose **More info**, then **Run anyway**.
To avoid the warning, right-click the zip before unpacking it, choose **Properties**, tick
**Unblock** and press OK.

### Opening the package on macOS the first time

Apple has not signed the package, because signing needs a paid Apple developer account. macOS
refuses to open the downloaded `OpenGP.app` until you allow it once:

- **macOS 15 and later**: open OpenGP once and close the warning. Then, in **System Settings >
  Privacy & Security**, choose **Open Anyway** next to the message about OpenGP, and confirm.
- **macOS 13 and 14**: right-click OpenGP in Finder and choose **Open**.
- **Either**: run `xattr -dr com.apple.quarantine <package folder>` in Terminal before opening it.

If OpenGP then says that macOS is running it from a temporary copy, move the whole package folder
in Finder (to your Documents folder, for example) and open OpenGP from there. The games OpenGP
builds on your Mac open normally.

## What is in it

```
OpenGP[.exe]            the launcher, which players open (OpenGP.app on macOS, with its Qt inside)
qt/, qt.conf            the Qt the launcher uses, on Windows and Linux. On Windows, Qt's DLLs are
                        next to OpenGP.exe, with Microsoft's Visual C++ runtime and Direct3D shader
                        compilers (d3dcompiler_47.dll, dxcompiler.dll and dxil.dll)
opengp-install[.exe]    the installer, for the command line
sdk/package.toml        the version, the pinned compiler, compile and link settings
sdk/include/            the runtime's headers
sdk/lib/                the runtime as static libraries, with SDL3 and Dear ImGui
sdk/games/<game>/       game.toml, recomp.toml, config/symbols.txt, patches.cpp, status.cpp
sdk/games/common/       the games' main() and the code they share
ISO/                    for your images
licenses/               the licences of the third-party code in the package
docs/, README.md, LICENSE, CHANGELOG.md, THIRD-PARTY-NOTICES.md
```

After installing, the folder also holds the games, `opengp-<game>[.exe]`, and `user/`. On Windows
it holds `toolchain/` too.

## Building the package

A package is a CMake install of a build configured with `OPENGP_PACKAGE=ON`. `cmake/package.cmake`
lays it out. The package workflow in `.github/workflows/package.yml` builds all three packages,
runs the tests, and checks each package with `tools/package_probe.py`. The probe builds the test
suite's recompiled fixture the way the installer builds a game.

The workflow then builds the launcher,
[opengp-launcher](https://github.com/OpenGP-dev/opengp-launcher) at `LAUNCHER_REF`, against the
same checkout of OpenGP and with the Qt that `QT_VERSION` names. It runs the launcher's tests,
installs it into the package, and opens the installed launcher once (`--version` and
`--smoke-test`). For a version tag, the workflow attaches the packages to the release. It runs for
every version tag, on main and pull requests when a CMake file, the installer or the probe changes,
and by hand (Actions > Package > Run workflow, or `gh workflow run package.yml --ref <branch>`):
run it on the release commit before tagging, since other changes can break a package too.

The workflow checks the launcher out with the repository secret `LAUNCHER_TOKEN`, because its own
token can't read another repository. It is a fine-grained personal access token whose only access
is read-only Contents on OpenGP-dev/opengp-launcher. Without it, the workflow stops at once.

**Windows.** Unpack the llvm-mingw release that `cmake/package.cmake` pins, then run:

```sh
export OPENGP_LLVM_MINGW=/path/to/llvm-mingw-20260922-ucrt-x86_64   # PowerShell: $env:OPENGP_LLVM_MINGW = ...
cmake --preset package-windows
cmake --build --preset package-windows
cmake --install build/package-windows        # -> out/OpenGP-windows-x64
```

**macOS.** On Apple Silicon, with Apple's Clang:

```sh
cmake --preset package-macos
cmake --build --preset package-macos
cmake --install build/package-macos          # -> out/OpenGP-macos-arm64
```

**Linux.** Build with GCC 12 on the oldest distribution the package supports. The installer links
the games against the player's system libraries, so the runtime must not need newer ones. Set `CC`
and `CXX` for the whole build, the launcher's below included.

```sh
export CC=gcc-12 CXX=g++-12
cmake --preset package-linux
cmake --build --preset package-linux
cmake --install build/package-linux          # -> out/OpenGP-linux-x64
```

Then build the launcher against the same checkout, and install it into the package folder, as the
launcher's README says under Building. The folder must be given as a full path: Qt's deployment
fails on a relative one. On Linux, do that with `CC` and `CXX` still set to GCC 12: the launcher
builds OpenGP's libraries too. On Windows, do it in OpenGP's dev shell (`tools/devshell.ps1`) with
the MSVC build of Qt, and add `-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`:

```sh
cmake -S <launcher> -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=<Qt>/6.10.3/<kit> -DOPENGP_SOURCE_DIR=$PWD -DLAUNCHER_DEV_HOME= -DLAUNCHER_NEWS_URL=
cmake --build build/launcher
cmake --install build/launcher --prefix $PWD/out/OpenGP-linux-x64     # or -windows-x64, -macos-arm64
```

To try a package build, open its launcher, for example `out/OpenGP-windows-x64/OpenGP.exe`, or run
its installer, `opengp-install`, with an image.

Some settings must change together:
- CI builds the runtime with the llvm-mingw release that `cmake/package.cmake` pins, and the
  Windows installer downloads that same release. Change its version and SHA-256 together.
- `cxx_flags` in `cmake/package.cmake` must match the flags the generated code needs, which
  `opengp_guest_flags` in `CMakeLists.txt` sets.
- The games' hand-written sources are listed once, in `cmake/game_sources.cmake`. The source build
  compiles that list, and the package writes it into `package.toml` for the installer.
- The link libraries in `cmake/package.cmake` must cover what the runtime and SDL3 need. The
  probe fails if one is missing.
- `LAUNCHER_REF` in the workflow names the launcher release for this OpenGP release. It is built
  against this OpenGP, whatever its own `external/OpenGP` submodule points to.
- `QT_VERSION` in the workflow is the Qt the packages carry. When it changes, make
  `thirdparty/qt/third-party.txt` again with `tools/qt_notices.py` from that Qt's source, and
  change the version in `THIRD-PARTY-NOTICES.md`.

## Releasing

1. Set the new version in `project(OpenGP VERSION ...)` in `CMakeLists.txt`. Every release needs a
   new one: the installer rebuilds the games only when the version changes.
2. In `CHANGELOG.md`, give the `## Unreleased` heading the version and the date, with a short
   summary under it.
3. In `README.md`, name the new version as the latest release.
4. Set `LAUNCHER_REF` in `.github/workflows/package.yml` to the tag the launcher release for it
   will have, such as `v1.0.0`.
5. Run the package workflow by hand on the release commit (Actions > Package > Run workflow, or
   `gh workflow run package.yml --ref <branch>`), and check that all three packages build. Until
   the launcher's tag exists, give its release branch as `launcher_ref`
   (`-f launcher_ref=<branch>`).
6. Release the launcher: merge its release branch into its `main`, tag the merge as `LAUNCHER_REF`
   says, and push the tag. The packages are built from that tag.
7. Merge the release branch into `main` with `git merge --no-ff`.
8. Tag the merge `v<version>`, the same version as in `CMakeLists.txt`, and push the tag. The
   workflow builds the packages again and attaches them to a draft release; it fails if the tag
   and the version differ. Check the draft, then publish it.
