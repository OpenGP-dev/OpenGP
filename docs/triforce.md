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

- It maps naturally onto SDL haptics.

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

OpenGP skips all of this:
- It loads `main.dol` itself.
- It fills in the low-memory boot globals that Segaboot would normally leave (at `0x80000000`:
  disc ID, memory size, console type, clocks, arena bounds).
- It answers the media-board commands directly.
