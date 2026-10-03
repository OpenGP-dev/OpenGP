# Triforce hardware notes

Working notes on the arcade hardware the runtime has to replace. Unless stated otherwise, the
reference implementation is Dolphin mainline (Triforce support was merged in 2512-395), under
`Source/Core/Core/HW/`.

## The board

The Triforce is a stock GameCube main board with two Triforce-specific boards attached:

| Board | Role | Dolphin source |
|-------|------|----------------|
| GameCube board | Gekko CPU (486 MHz), Flipper GPU, audio DSP, ARAM | Everything outside `Triforce/` |
| AM-Baseboard | JVS I/O over SI; serial ports (wheel force feedback, card reader, camera) | `SI/SI_DeviceAMBaseboard.cpp`, `Triforce/JVSIO.cpp`, `Triforce/IOPorts.cpp`, `Triforce/SerialDevice.cpp` |
| AM-Mediaboard | Holds the game image in DIMM memory, which the game reads through DI commands; also provides network sockets | `DVD/AMMediaboard.cpp` |

Clocks: the bus runs at 162 MHz, and the time base ticks at bus / 4 = 40.5 MHz, the same as the
GameCube.

**Open question:** main RAM size. Some sources say 24 MiB (as on the GameCube), others 48 MiB.
Check Dolphin's `SConfig::GetSimulatedMemorySize()` for Triforce titles before relying on either.

## The games

| Game | ID | Media | Notes |
|------|----|-------|-------|
| Mario Kart Arcade GP 2 | `GGPE02` | NAND | First target |
| Mario Kart Arcade GP | `GGPE01` | NAND | Second target; same Namco engine |

- Dolphin's media board tells the two games apart by the ID codes inside the image
  (`AMMediaboard.cpp` has a `s_game_map`).
- Both answer the DI inquiry command as "Version2" and are treated as NAND media.
- Raw NAND dumps become Dolphin-ready images with
  [FIX94/triforce-nand-iso-extract](https://github.com/FIX94/triforce-nand-iso-extract).
- Bad dump headers can be checked and fixed with
  [FIX94/triforce-header-patcher](https://github.com/FIX94/triforce-header-patcher).

## Media board (DI interface)

The game talks to the media board through DI commands; the command byte is the top byte of the
first command word.

| Command | Use |
|---------|-----|
| Inquiry | Protocol version |
| Read | DIMM game data, board status, and the execute and network buffers |
| Write | Firmware, DIMM, network configuration |

Status replies the game expects:
- DIMM size: 512 MiB
- Segaboot version
- System flags
- Board serial
- A "test hardware" command that answers in two phases

Network sockets (`Socket`, `Bind`, `Connect`, ...) are used for linked cabinets. They will be
stubbed first and connected to host sockets later.

## AM-Baseboard

### JVS I/O over SI

| Input | Type |
|-------|------|
| Steering | Analog |
| Gas | Analog |
| Brake | Analog |
| Item button | Digital |
| Start | Digital |
| Coin | Digital |
| Test / Service | Digital |

Button lamps (outputs) are also driven over JVS.

### Steering wheel force feedback

See `Triforce/MarioKartGP.cpp` in Dolphin.

- It is a serial device.
- Requests are 10-byte packets beginning `FF FF FF 01`, followed by three big-endian `u16`
  values: centering force, friction force and roll.
- The device acknowledges with a 3-byte ASCII code:

  | Ack | Meaning |
  |-----|---------|
  | `E00` | No error / power-up |
  | `C01` | FFB on |
  | `C06` | FFB off (end of race) |

- It maps naturally onto SDL haptics: centering as a spring, friction as friction, and roll (a
  signed push, see milestone 5 below) as a constant force.

### Magnetic card reader

- Stores player progress.
- Emulated as one save file per card (Dolphin: `HW/MagCard/`).

### NamCam2 camera

- Takes the player's photo, which is shown on their character.
- It hangs off the Namco JVS I/O board.
- First version: the check is disabled. Later: a webcam feed.

## Known game-side checks (GGPE02)

The community "skip checks" patch
([coremaven/Mario-Kart-Arcade-GP-2---Skip-Checks-Patch](https://github.com/coremaven/Mario-Kart-Arcade-GP-2---Skip-Checks-Patch))
changes 14 instructions in `main.dol`, for example at `0x8003737C` and `0x80084850`. It
bypasses:
- the 60-second satellite-link wait (a 3600-frame timer cut to 1 frame);
- NamCam2 camera validation;
- steering wheel initialisation errors ("Wheel error", `e20`);
- test-mode hardware validation and a stuck seat-initialisation loop.

OpenGP will implement these as `[[patch]]` / `[[hook]]` entries in `games/mkgp2/recomp.toml` once
the addresses are checked against the real DOL (milestone 3).

## Boot

On real hardware:
1. Segaboot (the Triforce IPL) initialises the media board.
2. It loads the game from DIMM into RAM.
3. It jumps to the DOL entry point.

OpenGP skips Segaboot, as Dolphin does:
- It fills in the low-memory boot globals that Segaboot would normally leave (at `0x80000000`:
  disc header, memory size, console type, clocks).
- It runs the disc's apploader, which loads the game from the image.
- It seeds the media board's command keys from the first 12 bytes of low memory (the disc
  header). The game's DVD driver obfuscates every DI command with keys that change after each
  command.
- The media board then serves the game's reads from the image and answers its board commands.

## Mario Kart Arcade GP 2 (GGPE02): what milestone 2 found

The images are standard GameCube disc images: magic `0xC2339F3D`, `main.dol` at `0x18000`,
readable with decomp-toolkit's `vfs` commands.

### Executable (`main.dol`, 5.03 MiB)

| Section | Address | Size | Notes |
|---------|---------|------|-------|
| `.init` | `0x80003100` | `0x24E8` | SDK startup (`__start` at `0x80003154` is the entry point), TRK vector table |
| `extab` / `extabindex` | `0x80005600` / `0x8001F340` | | C++ exception tables: the game engine uses C++ exceptions |
| `.text` | `0x8002BC40` | `0x2BC678` | 7,598 functions; game code first, SDK libraries from about `0x8025519C` |
| `.rodata` | `0x802E8380` | `0x10CC64` | |
| `.data` | `0x803F5000` | `0x108CA8` | |
| `.bss` | `0x804FDCC0` | `0x1D0CB8` | |
| `.sdata` / `.sdata2` | `0x806CE980` / `0x806D1EC0` | | `_SDA_BASE_` (r13) = `0x806D6980`, `_SDA2_BASE_` (r2) = `0x806D9EC0` |

decomp-toolkit finds 7,612 functions covering all of `.text`. About 900 are named:
- about 280 by decomp-toolkit's own signatures;
- 632 by `tools/sigmatch.py`, which uses Dolphin's signature database.

The game's `main` is at `0x800AC27C`.

### Libraries

- **Nintendo Dolphin SDK**, release build of 17 April 2003 (`0x2301`), per its own banners.
- **MusyX** (Factor 5) for audio: `synth*`, `snd*`, `sal*` and `mcmd*` functions. MusyX may upload
  its own DSP microcode rather than Nintendo's AX, which decides between DSP HLE and LLE in
  milestone 6.
- **MetroTRK** debugger stub (`TRK*`); needs to be stubbed out.
- Possibly **ODE** (Open Dynamics Engine) for physics: one signature matched `dBodyGetMass`.

### How far it runs (milestone 3)

`opengp-mkgp2` boots the image and runs the game into its main loop. On the way:
1. `OSInit` prints the kernel banners: "Development HW3", 48 MB, arena `0x806ECBE0`-`0x82FB7B80`.
2. It reads the baseboard's backup memory over EXI, one byte at a time.
3. It talks to the baseboard over SI: status switches, then JVS I/O board set-up.
4. HSD (HAL's library) and GX start ("hsd lib init for triforce").
5. It sends media board commands: DIMM size, SegaBoot version, serial number, network address.
6. AR, ARQ, AI and DSP start, and MusyX uploads its DSP microcode (CRC `0x3AD3B7AC`, 7,872
   bytes).
7. The main loop runs at 60 Hz, drawing about 160 primitives a frame and copying each frame to
   the external framebuffer.

### What it shows (milestone 4)

With frames rendered, the start-up sequence is:
1. **Device Check**: HANDLE, BUTTON, CARD and CAMERA, each "CHECKING" then "OK".
2. **LINK**: "PCB ID : 1 OK" while it looks for linked cabinets (IDs 2-4) for 60 seconds.
3. **Attract mode**: race footage, character introductions, "Please insert 1 coin(s)".

What it needed:
- **Camera**: the NamCam2 is an IP camera on the media board's network ("CAMERA IP INITIALIZE
  WAIT"). Sockets are not emulated, so the check ends in "CAMERA ERROR / Please call an
  attendant". The game can run without a camera: byte `0x25` of its settings block
  (`0x805986C0`) is "camera use", and when it is 0 the check is skipped. `recomp.toml` patches
  the settings defaults (`0x80073BD8`) to 0. Settings live in the baseboard's backup memory
  (`user/tribackup_GGPE02.bin`): a backup written with the camera on keeps it on, so delete that
  file once to get the new default.
- **Fonts**: the LINK screen's text uses the SDK's `OSInitFont`, which reads the system font
  from the boot ROM over EXI. Without it the game crashes on a null font. The IPL device serves
  free replacement fonts (`thirdparty/gc-fonts`), or the fonts from a boot ROM dump in
  `user/IPL.bin`.
- **EFB reads**: attract mode reads the EFB from the CPU (`0xC8000000`: depth peeks, likely
  for lens flares), about once a frame. Waiting for the GPU at each one halved the frame rate,
  so the first peek in a frame requests a readback of the EFB at that point, and peeks return
  the newest completed one, usually from the previous frame. `OPENGP_EFB_SYNC=1` makes them
  exact (and slow).
- **Spurious decrementer**: the SDK's `DecrementerExceptionCallback` calls `OSLoadContext`
  early when no alarm is due. `OSLoadContext` must not return there (see `os/threads.h`).

Notes:
- The `sc` instructions at the end of `DCFlushRange` and similar functions only synchronise the
  caches, so they are treated as no-ops.
- `RealMode` (used by `Config48MB` to reprogram the BATs) is a no-op: the memory map is fixed.
- The game's first DI command is a disc ID read into `0x80000000`, before which it clears that
  memory. That is why the media board keys must be seeded at boot rather than on the first
  command.

### Playing it (milestone 5)

With a coin, start and the menus, the game goes from attract mode into a race, and after the race
(or "TIME UP!") shows the Mario Coin results and goes back to character select. What that took:

- **Controls** (JVS, Namco FCA-1, as in Dolphin): start is switch byte 0 bit `0x80` of player 1,
  the item button byte 1 bit `0x20`, VS cancel byte 1 bit `0x02`; analog channels 0-2 are
  steering, gas and brake; coin slot 1 is the coin chute. The game counts coins from the JVS
  coin counter itself and never decrements it.
- **Analog ranges**: the game calibrates the wheel and pedals from its settings block
  (`0x805986C0`: steering centre `+0x34`, default `0x7FFF`; gas and brake rest positions `+0x40`
  and `+0x3C`, default 0), at start-up and in the test menu's handle set-up. Steering then spans
  the centre +/- `0x5000` (0 is full left), and a pedal reads from `0x800` above its rest position
  to `0x4000` further. The I/O board scales the controls to exactly that range
  (`triforce/mkgp.cpp`). The decomp names the setters `JvsInput_Set*Calibration`.
- **Force feedback**: centering and friction are 0 to about `0x7FFF` (a race uses `0x2800` and
  `0x1E00`); **roll is a signed push centred on `0x8000`**: `0x7F00`/`0x8100` alternate as the
  road's buzz, bumps and hits reach `0x5800`/`0xA800`. It is 0 while force feedback is off (menus).
- **Free play**: the coin settings are not the game's own but the arcade library's (SegaBoot's
  system settings, from the backup's `ACBU0001` block). Its set-up routine (`0x80289B54`) copies
  them to `0x80690AA0`; bytes `0xC`-`0xF` are the cost of a play per coin slot, and at 0 the game
  shows "CREDIT(S) 0/0" and "Step on gas pedal to start". `--free-play` clears them after the
  set-up runs.
- **LINK wait**: after the device check the game waits 60 seconds for linked cabinets (a
  3600-frame timer set at `0x80084850`). `recomp.toml` sets it to 1 frame (from the community
  patch).
- **Frame pacing**: the frame ends with one `VIWaitForRetrace`. Races need 10-14 ms of CPU a
  frame, so any delay in resuming the game thread after a retrace costs a whole frame. The idle
  loop in `SelectThread` takes interrupts at its back-edge and jumps straight back to the idle
  hook, which then waited again for the next interrupt with the game thread already runnable;
  races ran at 40-50 fps until the hook took the interrupt itself (`os/threads.cpp`).
- **Test switch** (solved in milestone 7): the game's service-button handler (`0x8002D050`,
  every frame) reads input bit `0x1000` (test: JVS system bit `0x80`, or baseboard status bit
  `0x40` inverted) and `0x0800` (service: player 1 switch bit `0x40`, or status bit `0x80`
  inverted). Service held and test pressed opens the I/O test (`ServiceMenu` page `0xB`). Test
  alone ends the main loop, and `main` then **reboots** into the test menu (`8002DC1C`): it keeps
  0x100 bytes at `811FFF00` (`OSSetSaveRegion`), the first word 1 ("wake test mode"), and calls
  `OSResetSystem(0, 0, 0)`. At the next boot `BootDispatcher` reads them back
  (`OSGetSavedRegion`) and starts the test menu (flow mode 12 instead of 3). The test menu's EXIT
  reboots the same way; on the cabinet these reboots go through SegaBoot's system menu, which
  then starts the game. OpenGP emulates a reboot by restarting the process (`os/reboot.h`); a
  reboot from a boot that kept memory (the test menu) boots afresh.
