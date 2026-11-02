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
then `C:\Program Files\LLVM`, then the Clang of a Visual Studio installation that has the Clang
component. Without a standalone LLVM it uses that installation's environment, even when a newer
Visual Studio without Clang is installed too. It stops if clang-cl is older than 16.

MinGW-w64 GCC may work (`$env:CC='gcc'; $env:CXX='g++'; cmake --preset default`), but it is not
tested: clang-cl is the supported configuration and what CI uses.

The first configure downloads SDL3, glslang and Dear ImGui at pinned versions, and on macOS
SPIRV-Cross too (see `OPENGP_BUILD_VIDEO` below). CMake builds
that use GnuTLS without a certificate store (the WinLibs/MinGW CMake, for one) fail those
downloads with "certificate signer not trusted": use Kitware's CMake
(`winget install Kitware.CMake`), or point CMake at a CA bundle, e.g.
`cmake --preset clang-cl "-DCMAKE_TLS_CAINFO=C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"`.

## Linux and macOS

```sh
python3 tools/setup.py
cmake --preset default           # uses $CC / $CXX (GCC or Clang)
cmake --build --preset default
ctest --preset default
```

On Linux, SDL3 is built with X11 support and needs its development packages (Debian/Ubuntu:
`libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev
libxtst-dev libxkbcommon-dev libasound2-dev libpulse-dev`; see SDL's
[Linux build notes](https://wiki.libsdl.org/SDL3/README-linux#build-dependencies)), and a
Vulkan driver at run time.

On macOS (Apple Silicon), install the Xcode Command Line Tools (`xcode-select --install`) and
CMake and Ninja (`brew install cmake ninja`). The renderer uses Metal: the shaders are also
converted to the Metal Shading Language at build time (with SPIRV-Cross, fetched at a pinned
version; `OPENGP_METAL_SHADERS`), so MoltenVK is not needed. An SDL3 from Homebrew is used if
installed, else the pinned release is built.

## Running

```sh
build/<preset>/games/mkgp2/opengp-mkgp2 [--iso PATH] [--user-dir PATH] [--timeout SECONDS] [--status SECONDS]
                                        [--fullscreen | --windowed] [--aspect RATIO] [--scale N]
                                        [--headless] [--no-video] [--screenshot FRAME]... [--screenshot-every N]
                                        [--input SCRIPT] [--input-file PATH] [--free-play] [--test-menu]
                                        [--no-audio] [--volume PERCENT] [--audio-dump PATH] [--game-log]
                                        [--camera off|webcam|test] [--cabinet 1-4] [--lan] [--verbose]
```

`opengp-mkgp1` (Mario Kart Arcade GP, in `build/<preset>/games/mkgp1/`) takes the same options. `--version` prints the
version and the game.

Controls and playing are covered in [playing.md](playing.md).

`opengp-mkgp2` finds the image in `ISO/` or `private/` (or takes `--iso`), boots it and runs the
game in a window (Vulkan, or Metal on macOS, through SDL3's GPU API). It prints a short summary at
start-up, what happens (a card, a gamepad, a restart) and problems. With `--verbose` it also prints:
- the game's `OSReport` output, the hardware's unhandled accesses and first register accesses, and
  the DSP's microcode;
- a status line every `--status` seconds (default 5 with `--verbose`, else none; 0 for none): retraces, frames and draws so
  far, guest threads, pending interrupts, the sound output's queue (its length, and how often it
  ran dry or overflowed), and each guest thread's call stack when call tracing is on.

The settings in `user/settings.toml` (see [playing.md](playing.md#settings)) apply to every run;
options that change them do so for that run only.

| Option | Effect |
|--------|--------|
| `--user-dir PATH` | Where saves, settings and screenshots go (default `user/`) |
| `--fullscreen`, `--windowed` | Start fullscreen or in a window |
| `--aspect RATIO` | `auto` (the window's shape), `4:3`, `16:9`, `16:10` or `21:9` |
| `--scale N` | Internal resolution: N times the native 640x528 (1-8) |
| `--headless` | Render without a window (for screenshots) |
| `--no-video` | Do not render at all (only the GPU's registers are emulated) |
| `--screenshot FRAME` | Save frame number FRAME to `user/screenshots/` (repeatable); F12 does the same while playing |
| `--screenshot-every N` | Save every Nth frame |
| `--timeout SECONDS` | Quit after this long; default: never with a window, 20 seconds otherwise |
| `--free-play` | No coins needed |
| `--test-menu` | Start in the game's test menu (see [playing.md](playing.md#the-test-menu)) |
| `--input SCRIPT`, `--input-file PATH` | Play scripted input (see [playing.md](playing.md#scripted-input)) |
| `--no-audio` | No sound (there is none with `--headless` or `--no-video` either) |
| `--volume PERCENT` | Sound volume, 0-100 (default 100) |
| `--audio-dump PATH` | Also write the sound to a WAV file (works headless too) |
| `--game-log` | Print the game's own debug messages (see [triforce.md](triforce.md#the-games-debug-messages-milestone-10)) |
| `--verbose` | Print what is only for working on OpenGP: the SDK's messages, hardware accesses, a status line every 5 s |
| `--camera SOURCE` | The camera's pictures: `off`, `webcam` or `test` (colour bars; see [playing.md](playing.md#the-camera)) |
| `--cabinet N` | This cabinet's number for linking, 1-4 (see [playing.md](playing.md#linked-cabinets)) |
| `--lan` | Link with cabinets on the local network too |

Files the machine writes (baseboard backup memory, media board settings, Mario Cards, the
settings and controls, screenshots) go in `user/`, which is gitignored.

Environment variables for debugging:

| Variable | Logs |
|----------|------|
| `OPENGP_LOG_DI=<n>` | the first *n* disc interface commands |
| `OPENGP_LOG_MEDIABOARD=1` | media board commands |
| `OPENGP_LOG_NETWORK=1` | the games' sockets on the media board's network (connections, binds, options), and the cabinets the link finds |
| `OPENGP_LOG_EXI=<n>` / `OPENGP_LOG_SI=<n>` | the first *n* EXI / SI transfers |
| `OPENGP_LOG_JVS=1` | the JVS I/O protocol |
| `OPENGP_LOG_FFB=1` | the forces the game sends to the steering wheel |
| `OPENGP_LOG_DISCORD=1` | what Discord Rich Presence shows, each time it changes; headless runs show it too (on Discord as well, if it runs) |
| `OPENGP_GPU_DEBUG=1` | enables the GPU API's validation (needs the Vulkan SDK's validation layers) |
| `OPENGP_GX_DUMP=<n>` | every draw (with its pipeline state), texture, EFB copy and clear of frame *n* |
| `OPENGP_EFB_SYNC=1` | (not a log) CPU reads of the EFB wait for the GPU instead of returning the previous frame's contents |
| `OPENGP_FRAME_LOG=1` | each frame's duration, time in draws and vertex count |
| `OPENGP_LOG_INTERP=1` | each guest address where code falls back to the interpreter (code the recompiler missed), once |
| `OPENGP_PROFILE=START,SECONDS` | (Windows) samples every thread for SECONDS seconds, starting START seconds in, then prints each busy thread's hottest functions, self and inclusive |
| `OPENGP_CHECK_HLE=1` | (not a log) runs the game's own version of each SDK function OpenGP replaces too, and prints any difference |
| `OPENGP_NO_CRASH_DIALOG=1` | (not a log) no message box after a crash on Windows: the report is still written (for tests and scripts) |
| `OPENGP_LAUNCHER=1` | (not a log) set by the OpenGP launcher for the games it starts: start-up messages point at the launcher instead of command-line options and go only to the log, with no message box (and on Windows, no "OpenGP has crashed" box after a crash): the launcher shows them itself. Messages the window could no longer show (a save that failed as the game closed or restarted) go to the log as `[unseen]` lines, which the launcher shows too |

The status line also shows where time goes: the guest thread's time in draws (pipeline state,
textures, vertex transform) and EFB readbacks, and the render thread's time rendering and idle.

After a crash, the game prints the faulting address (and the guest address, if the access
was into guest memory) and a backtrace of `module+offset` entries. Resolve them with
`llvm-symbolizer --obj=build/<preset>/games/mkgp2/opengp-mkgp2.exe --relative-address <offset>...`.

## Build options

| Option | Default | Effect |
|--------|---------|--------|
| `OPENGP_BUILD_TESTS` | `ON` | Build the test suite |
| `OPENGP_BUILD_GAMES` | `ON` | Build the games whose `main.dol` is present in `orig/` |
| `OPENGP_GENERATED_DEBUG_INFO` | `OFF` | Emit debug info for recompiled game code (large and slow) |
| `OPENGP_BUILD_VIDEO` | `ON` | Build the renderer. Needs SDL3 and glslang: installed copies are used if CMake finds them (`SDL3` package, `glslangValidator`), otherwise the pinned releases are downloaded and built |
| `OPENGP_TRACE_CALLS` | `OFF` | Track each guest thread's call stack in recompiled code, for the game's status output (slower) |
| `OPENGP_METAL_SHADERS` | `ON` on macOS | Also convert the shaders to the Metal Shading Language, with SPIRV-Cross (for SDL's Metal backend) |
| `OPENGP_BUILD_INPUT` | `OFF` | Build the controller and webcam libraries without the renderer (for the launcher project, which builds OpenGP's libraries as part of itself) |
| `OPENGP_PACKAGE` | `OFF` | Build the installer package instead of the games (see [packaging.md](packaging.md)) |
