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
| `Mmio` | Hardware registers (`0xCC000000`–`0xCC007FFF`) | Runtime MMIO handler |
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

MEM1 is 24 MiB on the GameCube. The Triforce's size is still to be confirmed against Dolphin, so
it is a runtime parameter.

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

## What comes next (milestones 2 onward)

| Area | Plan |
|------|------|
| OS HLE | Dolphin SDK threads become host threads, with only one guest thread running at a time (N64Recomp's "ultramodern" pattern). Alarms, interrupts and callbacks are delivered at safe points. |
| Hardware | Register-level models of VI, PI, MI, DSP, AI, SI, EXI, DI and CP/PE |
| GPU | Emulated at the FIFO level: CP/XF/BP state, vertex loader, texture decoders, one TEV ubershader, EFB copies. Backend: SDL3 GPU. |
| Audio | DSP microcode identified by hash; AX HLE ported from Dolphin |
| Triforce | Media board (DIMM reads served from the image), JVS I/O, wheel force feedback, magnetic card reader, backup memory; see [triforce.md](triforce.md) |
