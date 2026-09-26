# Building

You need:
- CMake 3.25+
- Ninja
- Python 3.9+ (for `tools/setup.py`)
- A C++20 compiler with weak-symbol support: Clang or GCC. On Windows, use **clang-cl**; MSVC's
  `cl` is rejected because recompiled code relies on weak symbols.

The games are only built when their executables have been extracted; see
[game-files.md](game-files.md). Without them you still get the recompiler, the runtime and the
test suite.

## Windows

1. **Visual Studio 2022 or later** (Community or Build Tools), with the "Desktop development with
   C++" workload. It provides the MSVC libraries and the Windows SDK.
2. **LLVM**, for clang-cl. Either:
   - install it from the [LLVM releases](https://github.com/llvm/llvm-project/releases)
     (`LLVM-<version>-win64.msi`, or extract `clang+llvm-<version>-x86_64-pc-windows-msvc.tar.*`
     into `%LOCALAPPDATA%\Programs\LLVM`); or
   - add Visual Studio's "C++ Clang Compiler for Windows" component.
3. **CMake and Ninja**, e.g. `winget install Kitware.CMake Ninja-build.Ninja`.

Then, in PowerShell:

```powershell
. .\tools\devshell.ps1           # enters the VS x64 environment and puts clang-cl on PATH
python tools\setup.py            # once: extracts main.dol from your images
cmake --preset clang-cl
cmake --build --preset clang-cl
ctest --preset clang-cl
```

`tools/devshell.ps1` looks for LLVM in `$env:OPENGP_LLVM`, then `%LOCALAPPDATA%\Programs\LLVM`,
then `C:\Program Files\LLVM`, then Visual Studio's bundled Clang.

MinGW-w64 GCC also works (`$env:CC='gcc'; $env:CXX='g++'; cmake --preset default`), but clang-cl
is the supported configuration and what CI uses.

## Linux and macOS

```sh
python3 tools/setup.py
cmake --preset default           # uses $CC / $CXX (GCC or Clang)
cmake --build --preset default
ctest --preset default
```

## Running

```sh
build/<preset>/games/mkgp2/opengp-mkgp2           # loads the game and reports
build/<preset>/games/mkgp2/opengp-mkgp2 --run     # experimental: jumps to the entry point
```

The hardware is not emulated yet (milestone 3), so `--run` stops early. It prints the game's
`OSReport` output and the hardware registers it touched.

## Build options

| Option | Default | Effect |
|--------|---------|--------|
| `OPENGP_BUILD_TESTS` | `ON` | Build the test suite |
| `OPENGP_BUILD_GAMES` | `ON` | Build the games whose `main.dol` is present in `orig/` |
| `OPENGP_GENERATED_DEBUG_INFO` | `OFF` | Emit debug info for recompiled game code (large and slow) |
