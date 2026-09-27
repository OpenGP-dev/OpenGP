# Architecture

OpenGP ports Mario Kart Arcade GP 1 and 2 by **static recompilation**. The game's PowerPC (Gekko)
code is translated into C++ once, at build time. That C++ is compiled together with a runtime
that replaces the Triforce hardware.

This document describes the design and records the key decisions.

## Why static recompilation

A matching decompilation means rewriting every function by hand until it compiles to identical
bytes. That takes years. Static recompilation is mechanical: once the recompiler and runtime
work, the whole game is translated at once.

A decompilation can still come later. The symbol files use the decomp-toolkit format, so a
decomp project can share them. Hand-written C++ can also replace individual recompiled
functions (see "Overriding functions" below).

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

`common/gekko/ops.inc` is an X-macro table that lists every Gekko instruction once, with its
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
| Falling off the end of a function into the next one (e.g. the `_savegpr_N` chains) | Tail call to the next function |
| Backward branch (loop) | Also polls for pending interrupts, so the runtime can deliver interrupts |

Instruction semantics are **not** duplicated in the generator:
- The generator emits a call to a helper in `runtime/ppc/ops.h`, with the operands as constants.
- The helpers are `inline`, so the compiler folds them down to the equivalent hand-written C++.
- The interpreter calls the same helpers.

Performance work comes later and is not needed for correctness. The plan is to keep guest registers
in C++ locals rather than in `ctx`, as XenonRecomp does.

### Address classification

Loads and stores go to one of four places:

| Kind | Addresses | Emitted as |
|------|-----------|------------|
| `Ram` | MEM1 (`0x80000000`, `0xC0000000`) | Direct big-endian access to host memory |
| `Mmio` | Hardware registers (`0xCC000000`–`0xCC007FFF`) and the CPU's window onto the EFB (`0xC8000000`–`0xC8FFFFFF`) | Runtime MMIO handler |
| `GatherPipe` | Write-gather pipe (`0xCC008000`) | Appended to the GPU FIFO |
| `Checked` | Anything not proven to be RAM | Cheap address check, then one of the above |

The gather-pipe case matters: the Dolphin SDK's GX vertex macros (`GXPosition3f32` and friends)
are inlined, so game code writes to the FIFO directly.

To pick the kind, the recompiler runs constant propagation over each function's control-flow
graph, tracking `lis` / `addi` / `ori` / `mr`:
- Addresses built from constants are classified exactly.
- `r1` (the stack), `r2` and `r13` (the small-data bases) always point to RAM.
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
  same physical pages: `memfd` on Linux, `CreateFileMapping` + `MapViewOfFile3` on Windows.
- The **locked cache** (16 KiB) sits at `0xE0000000`.
- Everything else is left unmapped, so a stray access crashes immediately instead of silently
  corrupting state.

MEM1 is 24 MiB on the GameCube. Its size is a runtime parameter: the machine takes it from the
disc's `bi2.bin`, which says 48 MiB for the Triforce games (clamped to 24-64 MiB, as Dolphin does).

### Floating point

Gekko FP behaviour is ported from Dolphin, which bases it on hardware tests:
- NaN propagation order: `frA`, then `frB`, then `frC`.
- Single-precision `frC` is rounded to 25 bits before multiplies.
- Single-rounding FMA for single-precision operations.
- `fres` / `frsqrte` use the exact hardware estimate tables.
- Load/store conversions between single and double are bit-exact.
- Quantized loads and stores (`psq_l` / `psq_st`) follow the GQR settings.

FPSCR exception flags are not tracked yet (see the TODO in `ops.h`). The rounding mode is used
by `fctiw`.

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
- a timer thread for timed events (retraces, DMA completion).

| Block | Model |
|-------|-------|
| VI | Beam position and display interrupts at 60 Hz |
| DSP | Mailboxes, the boot ROM's microcode handshake, ARAM and audio DMA. Microcode is identified by hash; its commands are logged but not run yet (milestone 6). |
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
- the media board, which serves the game image and answers board commands;
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
  frames through SDL3's GPU API (Vulkan). The EFB is a colour and depth render target (640x528
  times the internal resolution scale). `shaders/tev.frag` is the TEV ubershader, ported from
  Dolphin's, with its integer arithmetic. The shaders are GLSL, compiled to SPIR-V at build time.

A queue of at most two frames between the two paces the game if rendering falls behind.

## What comes next

| Area | Plan |
|------|------|
| GPU | Remaining GX features as games need them (depth textures, z-freeze, destination alpha with blending, logic ops); Metal |
| Input | Keyboard and controller mapping onto the cabinet controls (`Machine::set_controls`) |
| Audio | The game's MusyX DSP microcode, run by HLE or LLE |
| Network | The media board's socket API, for linking cabinets |
