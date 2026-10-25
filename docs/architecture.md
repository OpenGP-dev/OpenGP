# Architecture

OpenGP ports Mario Kart Arcade GP 1 and 2 by **static recompilation**. The game's PowerPC (Gekko)
code is translated into C++ once, at build time. That C++ is compiled together with a runtime
that replaces the Triforce hardware.

This document describes the design and records the key decisions.

## Why static recompilation

A matching decompilation means rewriting every function by hand until it compiles to identical
bytes. That takes years. Static recompilation is mechanical: once the recompiler and runtime
work, the whole game is translated at once.

The symbol files use the decomp-toolkit format, so a decomp project can share them. Hand-written
C++ can also replace individual recompiled functions (see "Overriding functions" below).

## Pipeline

```
private/GGPE02.iso ─extract─> main.dol ─decomp-toolkit (dtk)─> symbols.txt
                                  │                                │
                                  └──────> gekkorecomp <───────────┴── games/<game>/recomp.toml
                                               │
                                  build/generated/*.cpp  (derived from the game, never committed)
                                               │
                                   + runtime/ ─┴─> executable
```

1. **Extract.** `main.dol` is pulled out of the user's Dolphin-ready Triforce image.
   (Raw NAND dumps are first converted with FIX94's `triforce-nand-iso-extract`.)
2. **Analyse.** [decomp-toolkit](https://github.com/encounter/decomp-toolkit) finds function
   boundaries, jump tables and relocations. It writes the result to `symbols.txt`, which holds
   names and addresses only.
3. **Recompile.** `gekkorecomp` reads the DOL, `symbols.txt` and a TOML config, and writes C++.
4. **Build.** The generated C++ is linked with the runtime and the game's hand-written patches.

## Recompiler (`tools/gekkorecomp`)

### Instruction set

`common/gekko/include/gekko/ops.inc` is an X-macro table that lists every Gekko instruction once, with its
opcode, extended opcode and operand signature. The following are all generated from it:
- the decoder
- the disassembler
- the test encoder
- the interpreter's dispatch
- the recompiler's emitter

Adding or fixing an instruction therefore happens in one place.

### Code generation

Each guest function becomes one C++ function in `namespace guest`. This is real
output for a small `switch`:

```cpp
PPC_FUNC_IMPL(__imp__fn_switch) {
  const ::ppc::u32 entry_lr = ctx.lr;
  const ::ppc::u32 entry_sp = ctx.gpr[1];
  // 80003164: cmplwi r3, 0x4
  ::ppc::op::cmpli(ctx, 0, 3, 0x4);
  // 80003168: bgt 0x800031AC
  if (::ppc::bc_cond(ctx, 12, 1)) { goto loc_800031AC; }
  // 8000316C: lis r5, 0x8020
  ::ppc::op::addis(ctx, 5, 0, -32736);
  ...
  // 80003178: lwzx r0, r5, r0
  ::ppc::op::lwzx<::ppc::Mem::Ram>(ctx, base, 0, 5, 0);
  // 8000317C: mtctr r0
  ::ppc::op::mtspr(ctx, base, 9, 0);
  // 80003180: bctr
  switch (ctx.ctr & ~3u) { case 0x80003184u: goto loc_80003184; ... default: ::ppc::jump(...); return; }
loc_80003184:
  ...
}
PPC_WEAK_FUNC(fn_switch) { __imp__fn_switch(ctx, base); }
```

Control flow:

| Guest code | Generated C++ |
|------------|---------------|
| Branch inside the function | `goto` to a label |
| `bl` | Sets `ctx.lr`, then a direct C++ call |
| `b` into another function | Tail call followed by `return` |
| `bctr` through a jump table | `switch` on the target address |
| `bctrl`, `blrl`, unresolved `bctr` | `ppc::call()` / `ppc::jump()`: looks up a flat table of function pointers indexed by address, and falls back to the interpreter if nothing is found |
| `bl` or `b` into the middle of another function (e.g. the SDK's `_savegpr_N` entry points into `__save_gpr`) | That address becomes a function of its own: a copy of the code from there to the end of the function it is in |
| Falling off the end of a function into the next one | Tail call to the next function |
| Backward branch (loop) | Also polls for pending interrupts, so the runtime can deliver interrupts |

Instruction semantics are **not** duplicated in the generator:
- The generator emits a call to a helper in `runtime/ppc/ops.h`, with the operands as constants.
- The helpers are `inline`, so the compiler folds them down to the equivalent hand-written C++.
- The interpreter calls the same helpers.

Guest registers live in `ctx`, not in C++ locals as in XenonRecomp. The performance work is in the
runtime (see the CHANGELOG).

### Address classification

Loads and stores go to one of four places:

| Kind | Addresses | Emitted as |
|------|-----------|------------|
| `Ram` | MEM1 (`0x80000000`, `0xC0000000`) | Direct big-endian access to host memory |
| `Mmio` | Hardware registers (`0xCC000000`–`0xCC00FFFF`, except the gather pipe's `0xCC008xxx`) and the CPU's window onto the EFB (`0xC8000000`–`0xC8FFFFFF`) | Runtime MMIO handler |
| `GatherPipe` | Write-gather pipe (`0xCC008000`) | Appended to the GPU FIFO |
| `Checked` | Anything not proven to be RAM | Cheap address check, then one of the above |

The gather-pipe case matters: the Dolphin SDK's GX vertex macros (`GXPosition3f32` and friends)
are inlined, so game code writes to the FIFO directly.

To pick the kind, the recompiler runs constant propagation over each function's control-flow
graph, tracking `lis` / `addi` / `ori` / `mr`:
- Addresses built from constants are classified exactly.
- `r1` (the stack), `r2` and `r13` (the small-data bases) always point to RAM.
- A known RAM address plus an unknown index (an array or jump-table access) stays in RAM: reaching
  the hardware registers would need an index near `0x4C000000`.
- Everything else gets the `Checked` path.

### Config (`recomp.toml`)

- Inputs: the DOL and `symbols.txt`.
- Output: directory and number of files.
- Extra or overriding function definitions and jump tables.
- **Instruction patches**: replace an instruction word. They are applied both to the generated
  code and to guest RAM at boot.
- **Mid-function hooks**: call a C++ function before a given instruction, optionally skipping
  that instruction.

### Overriding functions

Every generated function is emitted twice:
- as `__imp__<name>`, which holds the recompiled code;
- as a *weak* `<name>`, which simply calls `__imp__<name>`.

The runtime or a game patch can define a strong `<name>` to replace the function (HLE). The
replacement can still call the original. This is the same technique as XenonRecomp:

```cpp
namespace guest {
PPC_FUNC(OSReport) {
  log_guest_string(ctx.gpr[3]);   // replacement
}
PPC_FUNC(SomeFunction) {
  __imp__SomeFunction(ctx, base);  // run the original, then adjust the result
  ctx.gpr[3] = 1;
}
}  // namespace guest
```

Hooks declared in `recomp.toml` are defined the same way, with `PPC_HOOK(Name) { ... }`.

## CPU runtime (`runtime/ppc`)

### Context

`ppc::Context` holds the complete Gekko user-visible state:

| Register(s) | Storage |
|-------------|---------|
| GPRs | 32-bit each |
| FPRs | Paired singles `ps0` / `ps1`, stored as raw 64-bit patterns so NaN payloads survive |
| CR | One 32-bit word |
| XER | `so`, `ov` and `ca` split out, plus the byte count |
| LR, CTR, FPSCR, MSR | 32-bit each |
| SPRs | A full SPR file: GQRs, HID2, WPAR, the locked-cache DMA registers, and so on |

### Memory

A 4 GiB host range is reserved, so a guest address is simply `base + addr`:
- **MEM1** is mapped twice, at `0x80000000` (cached) and `0xC0000000` (uncached), backed by the
  same physical pages: `memfd` on Linux, an unlinked `shm_open` object on macOS, and
  `CreateFileMapping` + `MapViewOfFile3` on Windows.
- The **locked cache** (16 KiB) sits at `0xE0000000`.
- Everything else is left unmapped, so a stray access crashes immediately instead of silently
  corrupting state.

MEM1 is 24 MiB on the GameCube. Its size is a runtime parameter: the machine takes it from the
disc's `bi2.bin`, which says 48 MiB for GP 2 and 24 MiB for GP (clamped to 24-64 MiB, as Dolphin
does).

### Floating point

Gekko FP behaviour is ported from Dolphin, which bases it on hardware tests:
- NaN propagation order: `frA`, then `frB`, then `frC`.
- Single-precision `frC` is rounded to 25 bits before multiplies.
- Single-rounding FMA for single-precision operations.
- `fres` / `frsqrte` use the exact hardware estimate tables.
- Load/store conversions between single and double are bit-exact.
- Quantized loads and stores (`psq_l` / `psq_st`) follow the GQR settings.

FPSCR exception flags are not tracked yet (see `runtime/ppc/fp.h`). The rounding mode is used
by `fctiw`.

Linked cabinets each run the race themselves and exchange only their controls (the game's link
is a lockstep relayed by the host cabinet), so every cabinet must compute the same guest results
to the bit, whatever its OS, CPU or compiler. What keeps them the same: the generated code is
built with `-ffp-contract=off` (no fused multiply-adds the guest didn't ask for), NaNs are chosen
explicitly as above, guest FMAs use a correctly rounded `fma`, and the host's floating-point
environment stays at its defaults. A change that breaks this breaks linked races between
platforms.

### Interpreter

A full interpreter over the same helpers serves two purposes:
- It is the **fallback** for code the recompiler missed.
- It is the **oracle** for the differential tests.

An interpreted "call" runs until control returns to the caller's LR with the stack pointer
restored.

## Testing

| Test | What it checks |
|------|----------------|
| `test_decoder` | Every instruction in the table survives an encode/decode round trip; known encodings disassemble to the expected text |
| `test_semantics` | Known answers for tricky instructions: carries, `divw` edge cases, rotate masks, FP conversion, estimate tables, quantization |
| `test_diff` | `fixturegen` writes a DOL of hand-written and randomly generated functions; `gekkorecomp` recompiles it during the build; each function then runs through both the interpreter and the recompiled code, which must end in bit-identical state (registers, memory, gather-pipe output) |
| `test_recomp_inputs` | The recompiler's inputs: decomp-toolkit symbol files, `recomp.toml`, DOL parsing, identifiers |
| `test_input` | Scripted input: parsing and timing |
| `test_dsp` | The ARAM accelerator's sample decoding and the AX microcode's voice mixing, on hand-made voices |
| `test_pause` | Pausing: the time base stops and carries on, timed events wait, guest code waits at safe points; retraces on the host's clock |
| `test_disc` | Disc images: SHA-1 known answers, finding the executable, files by path |
| `test_texture` | Telling mipmaps that fade with distance (an effect) from plain downscaled copies |
| `test_vertex_pipeline` | The GX vertex pipeline on random formats, matrices, lights and primitives: the same output every run, and bit for bit on x86-64 |
| `test_text` | The games' text on the console |
| `test_settings` | `settings.toml`, the card folder and reboot files |
| `test_controls` | `controls.toml`: reading, changing bindings as the menu does, writing it back |
| `test_camera` | The NamCam2 stand-in and its JPEG pictures, on the media board's network |
| `test_discord` | Discord Rich Presence's messages and client, against a stand-in for Discord |
| `test_link` | Linked cabinets on one computer: finding each other, connecting, versions, refusing strangers |
| `test_paths` | Folders named in other scripts ("José Ж 日本"): settings, cards, reboot files, images |
| `test_installer` | The installer: `package.toml` from this release and from 1.0.0 (which lists no sources), a compiler download that a server will not resume, and a package folder it cannot write to (not checked as root, who can write anywhere) |
| `crash_reports` | `crash_probe` crashes on purpose four ways; each must write `crash-report.txt` and exit with code 3 |

The differential test exercises the whole pipeline end to end: DOL parsing, symbol import,
analysis, codegen, dispatch, and the interpreter.

## The machine (`runtime/machine.h`)

`opengp::Machine` puts an emulated Triforce around the recompiled game. The launcher creates one,
boots the disc image, installs the recompiled module and runs it.

### Boot (`runtime/os/boot.cpp`, `runtime/disc/`)

The boot ROM is emulated the way Dolphin does it:
1. Low memory gets the globals the IPL leaves: disc header, memory size, console type, clocks.
2. The disc's apploader runs in the interpreter and loads the game from the image.
3. The game starts at its DOL entry point.

### Hardware (`runtime/hw/`)

Each register block at `0xCC000000` is a `hw::Device`. `hw::Hardware` routes MMIO accesses from
guest code to the devices, and owns:
- the PI interrupt controller;
- the time base and decrementer, which run in real time (40.5 MHz);
- the write-gather pipe;
- a timer thread for timed events (retraces, DMA completion);
- the pause (`set_paused`): the time base stops and carries on where it stopped, timed events
  wait, and interrupts are made pending so the guest thread reaches a safe point, where it
  waits until the machine resumes.

| Block | Model |
|-------|-------|
| VI | Beam position and display interrupts at 60 Hz |
| DSP | Mailboxes, the boot ROM's microcode handshake, ARAM and its DMA, the audio DMA (which hands each 5 ms buffer to the sound output as it starts), and the microcode: identified by hash, AX is emulated at a high level (see below) |
| AI | Streaming sample counter and its interrupt |
| SI | Transfers through the communication buffer to the baseboard on port 0 |
| EXI | IPL chip (RTC, SRAM, debug UART) and the baseboard's backup memory |
| DI | Command registers and DMA; the media board answers the commands |

Device state is guarded by one recursive mutex, shared by the guest thread and the timer thread.

### Guest threads (`runtime/os/threads.cpp`)

Every guest thread runs on its own host thread, but only the one holding the CPU token runs
guest code (N64Recomp's "ultramodern" pattern):
- `OSLoadContext` is replaced. Switching threads hands the token over; a context never seen
  before gets a new host thread.
- Interrupts are delivered at safe points: loop back-edges, and when MSR[EE] is turned on. The
  current registers are saved into the current `OSContext`, and the handler runs from the OS
  exception table.
- The scheduler's idle loop waits for an interrupt instead of spinning.

### Triforce boards (`runtime/triforce/`)

Ported from Dolphin; see [triforce.md](triforce.md):
- the media board, which serves the game image and answers board commands, including the socket
  API of its network (`network.cpp`, below);
- the baseboard on SI: JVS I/O board, steering wheel, magnetic card reader;
- the I/O ports that map player controls onto cabinet inputs.

### GPU (`runtime/gx/`, `runtime/video/`)

The command processor reads the GX FIFO as the CPU fills it, and a decoder walks the command
stream: CP/XF/BP register loads, display list calls, primitives. The pixel engine raises the
draw-done and token interrupts that games wait on.

Rendering is split in two, with plain data (`gx::Frame`, `runtime/gx/frame.h`) in between:

- **Front end** (`gx::Frontend`, on the guest thread). It keeps the BP, XF and TMEM state and
  does everything that needs guest memory at draw time:
  - Vertices are loaded (`vertex_loader.cpp`) and transformed on the CPU (`transform.cpp`,
    ported from Dolphin's software renderer): matrices, projection, lighting, texture coordinate
    generation, culling. The host GPU gets clip-space positions, lit colours and texture
    coordinates, so its vertex shader only passes them through.
  - Textures are decoded to RGBA8 (`texture.cpp`) and cached by content hash.
  - Each draw's pixel pipeline state becomes the uniforms of one TEV ubershader plus a small
    pipeline key (blending, depth).
  - EFB copies stay on the GPU. A copy's texture is registered at its destination address, and
    a later texture read there uses it for as long as the memory there is unchanged (copies never
    write guest memory). A copy to the XFB ends the frame.
- **Renderer** (`video::Renderer`, on the main thread, which owns the window). It runs the
  frames through SDL3's GPU API: Vulkan, or Metal on macOS. The EFB is a colour and depth render
  target (640x528 times the internal resolution scale). `shaders/tev.frag` is the TEV ubershader,
  ported from Dolphin's, with its integer arithmetic. The shaders are GLSL, compiled to SPIR-V at
  build time and, for Metal, converted to the Metal Shading Language with SPIRV-Cross.

A queue of at most two frames between the two paces the game if rendering falls behind.

The renderer presents a frame as soon as it has drawn it, unless **even frame pacing** is on
(`[video] even_pacing`): then the launcher presents it at the game's first retrace after the
game finished it (`hw::ViDevice::next_retrace_ns`, from the display interrupts' schedule on the
host's clock), as a cabinet's display would start showing its XFB. The renderer gets the
window's image first and waits only to submit (`Renderer::redraw(at_ns)`). The game's frames
take more or less time, but its retraces are evenly spaced, so a display with a variable refresh
rate shows the frames evenly; it costs half a frame of latency on average.

**Widescreen** (`Frontend::set_widescreen`) widens the EFB by the same number of pixels on each
side (107 for 16:9), with the game's 640 pixels in the middle:
- The front end maps the game's clip space onto the wide EFB without changing a draw's scale or
  place, so the host no longer clips 3D scenes at the game's screen edges: races show more on the
  sides (the game draws the scenery around the camera, so little is missing there).
- 2D (orthographic) draws stay in the middle, at their shape. Ones that span the game's whole
  width are stretched to the wide EFB's: full-screen backgrounds, fades, and the rows of tiles that
  menu backgrounds are drawn as (a run of draws of the same height, each starting where the last
  ended). Other 2D that reaches the game's screen edge (a banner running off the side) gets quads
  out to the wide edge that repeat its edge texels.
- Some full-screen overlays are drawn in 3D: a flat quad facing the camera (all its vertices at the
  same depth) that spans exactly the game's width, such as GP 2's rain. They are widened like 2D,
  and a repeating texture carries on across the added sides, so the pattern keeps its size.
- Scissors, clears and EFB copies that span the game's width span the wide EFB, so a full-screen
  copy (and the copy to the XFB, which is displayed) is as much wider. CPU reads of the EFB see
  the game's part.

### Audio (`runtime/dsp/`, `runtime/audio/`)

The game's sound library is MusyX, which drives Nintendo's AX microcode on the DSP (CRC
`0x3AD3B7AC`, which Dolphin also treats as AX). The microcode is emulated at a high level, ported
from Dolphin's AX HLE:

- Every 5 ms the game sends a command list. `dsp::AxUcode` (`dsp/ax.cpp`) walks the voices'
  parameter blocks in main RAM; each running voice's samples are read from ARAM through the
  accelerator (`dsp/accelerator.cpp`: ADPCM or PCM decoding, looping), resampled to 32 kHz,
  shaped by its envelope and low-pass filter, and mixed into the main and two effects buses.
  MusyX runs its effects on the CPU; the final stereo mix goes to main RAM.
- The audio DMA plays those buffers. As each one starts, `hw::DspDevice` hands its samples to a
  sink: `audio::HostOutput` (`audio/host.cpp`, SDL3) and/or a WAV file (`--audio-dump`).
- The audio DMA runs on the time base, so the game makes sound in real time. The DMA is
  scheduled from where the previous buffer ended, never from when the timer event ran, so the
  timer thread's lateness does not slow the sound down. The output keeps a queue of about 30 ms
  to absorb the 5 ms bursts, and holds it there by resampling up to 0.5% faster or slower,
  since the sound card's clock drifts against the host clock.

Not emulated: the polyphase resampler's coefficient table (it is in the DSP's ROM; linear
interpolation is used instead, as Dolphin does without the ROM), and AX's initial time delay (a
phase shift for 3D sound).

### The launcher (`runtime/app/`)

The games' executables only describe the game (`app::Game`: its ID, its recompiled module, how to
turn free play on); `app::run` does the rest:
- the command line and the player's settings (`settings.h`: `user/settings.toml`);
- the user folder's lock (`paths.h`: `user/.lock-<game ID>`, held while the game runs, so a second
  copy of the same game can't share the folder's saves; the other game may);
- booting, the status line, crash reports (`os/crash.h`: written to `user/crash-report.txt` from
  the crashing thread with no allocation, with a watchdog that ends the process if writing the
  report hangs), and hang reports (`hang_watch.h`: when no frame comes for 10 seconds, each guest
  thread's call stack, from its stack's back chain, to `user/hang-report.txt`);
- the window (fullscreen, and the aspect ratio and internal resolution that follow its size), the
  controls, the sound;
- the menu (`menu.h`): Dear ImGui, drawn by the renderer over the presented picture
  (`video::Overlay`), with the settings and the Mario Card the player holds. Settings changed there
  apply at once; they are saved when the menu closes and at exit, except those set on the command
  line.

### The installer package (`tools/installer/`)

Releases ship everything but the games: the runtime as static libraries with its headers, the
recompiler (inside the installer) and the games' own files. The installer checks the player's image,
recompiles its `main.dol`, compiles the result with a C++ compiler (llvm-mingw on Windows, the
system's on Linux and macOS) and links it with the runtime. It is a library: the program players
open, `OpenGP` (from opengp-launcher), builds the games with it, and so does `opengp-install`, its
command-line program. See [packaging.md](packaging.md).

### Reboots (`runtime/os/reboot.h`)

Games restart themselves with `OSResetSystem`, keeping a region of memory for the next boot (the
Triforce games use it to enter and leave their test menus). OpenGP replaces `OSResetSystem` (see
`games/common/sdk_hle.cpp`) and restarts the whole process: the launcher writes the reset code and
the kept memory to `user/reboot-<game ID>.bin` and starts itself again with the same options;
the new process hands them to the game's `OSInit` (`__OSGetExecParams`). Frame numbers (for
scripted input and screenshots) carry on across the restart. The new process does not inherit the
old one's sockets: they close as the old process ends or is replaced, so the new one can listen on
the same ports.

### Input (`runtime/input/`)

The main thread reads the player's devices once a frame and hands the result to the machine
(`Machine::set_controls`), where the JVS I/O board picks it up on the guest thread:

- `input::HostInput` maps the keyboard, SDL gamepads and other joysticks (wheels, pedals) onto
  the cabinet's controls (`triforce::Controls`), from bindings in `user/controls.toml`. It also
  plays the cabinet's outputs (`Machine::outputs()`: the wheel's forces) as SDL haptic effects or
  gamepad rumble.
- `input::Script` plays controls by frame number, for runs without a player. Its events are
  applied on top of the player's controls, keyed to the frame the GPU has reached.

### The network and the camera (`runtime/triforce/network.h`, `runtime/camera/`)

The games reach the media board's network through board commands (socket, connect, select,
send, recv...). `triforce::Network` numbers the sockets 1-63 as the board does and backs each with
a host socket, or with a `Service` that OpenGP plays itself: a connection to a service's address
runs in-process, with no host network. Host sockets are non-blocking; the games poll them once a
frame. Besides services, connections reach other cabinets (below) and routes added for tests;
others are refused, and binds go to the loopback interface.

Linked cabinets (M12): `Network::enable_link` makes each OpenGP one cabinet, numbered 1-4 like
the games' PCB ID (`app::Game::set_cabinet` sets the game's own setting, `games/common/cabinet.cpp`).
Cabinet n is at 192.168.29.150 + n - 1 and listens on one of ports 5000-5008; on the host, its
port 5000 + k is `[link] port` + 10 (n - 1) + k, so four run on one computer, and port + 10 (n - 1)
+ 9 takes UDP beacons. Twice a second each cabinet sends the others (on this computer, and with
`lan` to every network's broadcast address and to each cabinet found there) its number, its game
and the ports it listens on, and a last one when it stops. The beacons have a thread of their own
and start as soon as the game has its number, because the game looks for the others for only a
moment as it starts (below): they must be known by then. A game's connection to another cabinet goes to the host port only once that cabinet has said it
listens there, so a connection never waits for a cabinet that is not there, and it starts with a
16-byte hello naming the cabinet and the game: the cabinet accepting it hands it to its game only
then, as a connection from the other cabinet's address. Link connections send each write at once
(`TCP_NODELAY`): the games send one small message a frame, and with Nagle's algorithm each would
wait for the other computer's delayed acknowledgement, so messages between computers came in
bursts. With `lan` off, everything stays on the loopback interface. `Network::link_status()` is what the menu shows.

`Network::linked()` says whether the game has linked to another cabinet since start-up: a
connection to one made, or one accepted (services don't count). The launcher pauses only a game
that has not: other cabinets need a linked one to keep running (M12). A linked game that
loses its link stops with a link error until the cabinet restarts (`app::Game::link_error`
reads the flag); the launcher restarts it.

The camera is such a service (`camera::NamCam`, at 192.168.29.104-107 port 80): it answers the
game's `GET /img.jpg` with a 320x240 JPEG from its `camera::Source`, encoded by `camera/jpeg.cpp`
(baseline 4:2:0, which the games' own decoder reads). The sources are a test pattern and
`camera::Webcam` (SDL3's camera API), which its thread opens when the game asks for a picture
and closes 5 seconds after the last; while it is open, the thread keeps the newest frame cropped
to 4:3 and scaled. The launcher starts the source the settings ask for and tells the game whether it has a
camera (`app::Game::set_camera`), which sets the game's own "camera use" setting at start-up
(`games/common/camera_use.cpp`).

### Discord Rich Presence (`runtime/discord/`, `games/*/status.cpp`)

Each game reads what the player is doing from its globals (`game_status::read` in
`games/<game>/status.cpp`) into an `app::GameStatus`: the scene (start-up, the attract mode, the
menus, a race, the race over, the test menu) and, once chosen, the character, cup, course, place
and lap. The scene is the game's own `g_currentSceneState`, which it sets at every scene change
for linked cabinets to compare; the race globals have the same names and meanings in both games,
and the names of their characters, cups and courses are the games' own (`games/common/status.h`).

Once a second the launcher reads the status, and `app::RichPresence` (`app/rich_presence.h`) turns
it into Discord's two lines and pictures. `discord::Presence` sends them from a thread of its own
over Discord's local connection (`discord/ipc.h`: a named pipe or a Unix socket carrying framed
JSON; no Discord SDK), connecting while Discord runs, reconnecting when it restarts, and staying
within Discord's rate limit. The pictures are art assets in OpenGP's Discord application, uploaded
there from the games' textures; nothing from the games is in the code.

## Scope

The GPU emulation leaves out some GX features (depth textures, z-freeze, destination alpha with
blending, logic ops) that neither game is known to need.
