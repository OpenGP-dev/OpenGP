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

Main RAM: the machine takes its size from the disc's `bi2.bin`, as Dolphin does. Mario Kart
Arcade GP 2 asks for 48 MiB, Mario Kart Arcade GP for 24 MiB (the GameCube's).

## The games

| Game | ID | Media | Notes |
|------|----|-------|-------|
| Mario Kart Arcade GP 2 | `GGPE02` | NAND | First target |
| Mario Kart Arcade GP | `GGPE01` | NAND | Second target (milestone 8); same Namco engine and arcade library |

- Dolphin's media board tells the two games apart by the ID codes inside the image
  (`AMMediaboard.cpp` has a `s_game_map`).
- Both answer the DI inquiry command as "Version2" and are treated as NAND media.
- Raw NAND dumps become Dolphin-ready images with
  [FIX94/triforce-nand-iso-extract](https://github.com/FIX94/triforce-nand-iso-extract).
- Bad dump headers can be checked and fixed with
  [FIX94/triforce-header-patcher](https://github.com/FIX94/triforce-header-patcher).

## Media board (DI interface)

The game talks to the media board through DI commands. It writes the command words obfuscated:
after they are XORed with the evolving keys, the command is the low byte of the first word (the
plain drive fallback moves it back to the top byte).

| Command | Use |
|---------|-----|
| Inquiry | Protocol version |
| Read | DIMM game data, board status, and the execute and network buffers |
| Write | Firmware, DIMM, network configuration |

Reads from the game image may run past its end, and the bytes there read as zeros. The games
read whole 32-byte blocks: GP 2's last file, `_koopa_fire2.dat` (Bowser's fire), ends 2 bytes
before the image does, and loading it reads 20 bytes past the end. A read must not fail. The
DVD library on the Triforce never asks the drive what went wrong, because its
`DVDLowRequestError` only returns, so the game waits for that read forever.

Status replies the game expects:
- DIMM size: 512 MiB
- Segaboot version
- System flags
- Board serial
- A "test hardware" command that answers in two phases

Network sockets (`Socket`, `Bind`, `Connect`, ...) are used by the camera and for linked
cabinets. `triforce/network.cpp` runs them (milestone 11, from Dolphin's media board): each game
socket is a host socket, or a connection to a service OpenGP answers itself (the camera). Host
sockets are non-blocking, since the games poll them once a frame (`select` with a zero timeout);
only connecting waits, up to the game's timeout. Connections to other cabinets go to the
OpenGPs playing them (milestone 12, below); others are refused, and binds go to the loopback
interface. Details the games rely on:
- A reply's second word is the result, stored little endian, except `InetAddr`'s, which the game
  reads without swapping: the address in network byte order.
- Addresses are VxWorks' `sockaddr_in` (length, family, port and address in network byte order,
  8 bytes of padding); `fd_set`s are 256 bits, socket *n* at bit *n* % 8 of byte *n* / 8; a
  `timeval` is two little-endian words (seconds, microseconds).
- Timeouts (`SetTimeOuts`) are in milliseconds: 2000 for the camera, 6000 and 10000 for the link.

What the games do with the network, from the mkgp2-decomp's names, checked against GGPE02 (GP 2's
addresses):
- The game's side is `ac*` (`acSocket`, `acConnect`, `acSend`, `acRecv`, `acSelect`,
  `acSetTimeOuts`, ... at `0x80291xxx`), which sends the socket commands to the media board.
- A cabinet's own address is `192.168.29.150` + (PCB ID - 1), set at start-up with
  `acModifyMyIPaddr` and netmask `255.255.255.0` (`PcbIdToIpAndModify`, `0x80073954`). The PCB
  ID is byte `0x21` of the settings block (`0x805986C0`). The other cabinets are at the same
  addresses for their IDs.
- A cabinet's camera is at `192.168.29.104` + (PCB ID - 1) (`PcbIdToIpString`, `0x80073B24`;
  both lists are `g_pcbIpTable`, at `0x803FD910`; GP `0x8036B790`).
- The link between cabinets is `TCPConn_*`, `PcbConn_*`, `PCBComm_*`, `PcbSync_*` and
  `CommBroadcast_*` (each kart's lap and position); the test menu's network pages are
  `NetDiag_*`.
- The game gives each connection attempt a timeout (6 seconds in GP 2, per the decomp's author).
  Dolphin's media board blocks for that long, which freezes the whole game; naari3's Dolphin
  branch `feature/ammb-async-connect` (not merged) makes connect, send, receive and accept
  non-blocking.
- With sockets working, GP 2's link starts after the device check even on a lone cabinet: it
  listens on port 5000 and tries each other cabinet's ports 5008 down to 5000, over and over.

### Linked cabinets (milestone 12)

- The PCB ID is read in `GameBoot_Init` (`0x80075324`), which gives the cabinet its address
  inline (`acModifyMyIPaddr` at `0x80075A98`, from the table of the cabinets' and cameras'
  address strings at `0x803FD910`; GP `0x8036B790`) before it returns; `PcbIdToIpAndModify` does
  the same from the test menu. OpenGP sets its cabinet number when the game calls
  `acModifyMyIPaddr` (`games/common/cabinet.cpp`).
- `TCPListen_Poll` (`0x800876B0`) listens on port 5000 + k, k counting up to 8 and round again
  each time it listens anew, which is why the others try ports 5008 to 5000 in turn
  (`TCPConn_Poll`, `0x80086BC0`). Both `acConnect` and `acAccept` are asynchronous: the game polls
  `acPollResponse` once a frame, then reads the result.
- A cabinet looks for the others only as it starts, during its LINK screen: `BootPCBCheck_Init`
  (`0x800847E0`) sets a 3600-frame timer (`0x806D0E1C`) and `BootPCBCheck_Poll` (`0x800845DC`)
  counts it down, ending it early once all four cabinets' slots have answered (GP: `0x801B0DB8`,
  `0x801B0BD4`, timer `0x805B8958`). After that it only accepts the others' connections, and
  connects back to a cabinet that connected to it. So a cabinet started later links to those
  already running. OpenGP skips the wait (about a second and a half of looking, a port tried every
  100 ms), unless the beacons have found other cabinets that the game is not linked with yet: then
  it waits until it is, up to 15 seconds, trying again after a connection that failed on the way
  (`games/common/cabinet.cpp`).
- Each pair of linked cabinets has two connections, one made by each, and each carries data one
  way. Messages are a 16-bit type, a 16-bit size (including the 4-byte header) and the data
  (`TCPConn_PeekRxMessage`); a cabinet's first message is 120 bytes in GP 2 (GP: 96 and 80).
- Starting a game on one cabinet ("Number of entry", "Waiting for other challenger") makes the
  others show "Waiting for entry!"; players who join race in Versus mode, the cabinets moving
  from scene to scene together (the class, the characters, the items, the race).
- When a cabinet stops, the others stop their game with "Please call an attendant / Link error
  occurred" (`DrawLinkErrorOverlay`, `0x80085A34`, drawn while `0x806D0DF1` is set; GP: the
  same overlay at `0x801B1D90` with `0x805B8935`) until restarted.

With `--game-log`, the games print their network library's messages, which name each media
board result (`AC_MEDIACMD_ERR_CANNOT_OPEN`, ...) and describe it in Japanese.

## NamCam2 camera

- Takes the player's photo when a game starts ("Please align your face within the frame"), which
  is shown on their character in the race. GP asks first ("Take your picture for solo play?").
- An HTTP server on the media board's network, at the camera address above. For a photo
  `NamCam_GetImageHandle` (`0x800487F4`, a state machine run once a frame) connects to its port
  80 with a 2-second timeout, sends `GET /img.jpg HTTP/1.0` (26 bytes), then `select`s and
  `recv`s up to 64 KiB a frame into a 512 KiB buffer (giving up after 60 empty polls) until the
  camera closes the connection. The reply must end with JPEG's `FF D9`; the picture starts after
  the headers' blank line.
- The picture is a **320x240 JPEG**: the game decodes it itself (`Asset_LoadAndDecompressByType`,
  `0x8004861C`, decoder type 3) into a 320x240 RGBA8 texture it allocates (`0x4B000` bytes). Its
  decoder (from `0x8029A944`) takes baseline JPEG only (`SOF0`, 8 bits, 1 or 3 components with
  4:4:4, 4:2:2 or 4:2:0 sampling), and skips `APPn` and `COM` segments.
- The camera is powered through the wheel's force feedback board (`StrPcb`): at start-up
  `BootDispatcher` calls `StrPcb_BeginEffect` (`0x80037BCC`, with the PCB ID - 1), which starts a
  sequence of states 1-4 at `+0x60`, counted down in seconds at `+0x64` by `StrPcb_StateTick`
  (`0x80036864`): 20 seconds in state 2, the power bit set, 20 more in state 3. The start-up check
  waits for state 4 (`StrPcb_IsInErrorState`, in fact "powered") or 3000 frames.
- `CameraChecker_*` (from `0x8008B918`) is the start-up check: it waits for the power, calls
  `NamCam_Init`, and downloads a picture (3 tries of 300 frames). The test menu's camera page
  (`CameraTest_Page`) shows the camera's pictures live.
- OpenGP answers as the camera (`camera/namcam.cpp`) with its own JPEG encoder
  (`camera/jpeg.cpp`, baseline 4:2:0) and pictures from a webcam (`camera/webcam.cpp`, SDL3's
  camera API) or a test pattern. With the camera on, `games/common/camera_use.cpp` sets the
  settings' "camera use" byte after start-up reads them (`GameBoot_Init`, `0x80075324`; GP
  `0x8007A5CC`) and cuts the power sequence's waits to half a second; with it off, it clears the
  byte, so the check is skipped. GP's camera code is the same as GP 2's.

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

## Known game-side checks (GGPE02)

The community "skip checks" patch
([coremaven/Mario-Kart-Arcade-GP-2---Skip-Checks-Patch](https://github.com/coremaven/Mario-Kart-Arcade-GP-2---Skip-Checks-Patch))
changes 14 instructions in `main.dol`, for example at `0x8003737C` and `0x80084850`. It
bypasses:
- the 60-second satellite-link wait (a 3600-frame timer cut to 1 frame);
- NamCam2 camera validation;
- steering wheel initialisation errors ("Wheel error", `e20`);
- test-mode hardware validation and a stuck seat-initialisation loop.

OpenGP took two of its addresses, as `[[patch]]` entries in `games/mkgp2/recomp.toml` (GP has the
same two at its own addresses): the LINK screen's wait (`0x80084850`) and the default of the
"camera use" setting (`0x80073BD8`). Both are now only defaults: `games/common/cabinet.cpp`
lengthens the wait while other cabinets are linking, and `games/common/camera_use.cpp` sets the
camera setting at start-up. The game passes the other checks on OpenGP's emulated hardware, so
OpenGP doesn't take them.

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

decomp-toolkit finds 7,612 functions covering all of `.text`. 3,427 are named:
- about 280 by decomp-toolkit's own signatures;
- 632 by `tools/sigmatch.py`, which uses Dolphin's signature database;
- 2,512 by `tools/decompnames.py`, from the mkgp2-decomp project (see
  [game-files.md](game-files.md#regenerating-the-symbol-list)).

Of the data, 488 objects are named, 372 of them from the decomp.

The game's `main` is at `0x800AC27C`.

### Libraries

- **Nintendo Dolphin SDK**, release build of 17 April 2003 (`0x2301`), per its own banners.
- **MusyX** (Factor 5) for audio: `synth*`, `snd*`, `sal*` and `mcmd*` functions. It uploads
  Nintendo's stock AX microcode, which OpenGP emulates at a high level (milestone 6).
- **MetroTRK** debugger stub (`TRK*`): present, but never reached, so nothing replaces it.
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
  WAIT"). Without sockets the check ended in "CAMERA ERROR / Please call an attendant". The
  game can run without a camera: byte `0x25` of its settings block (`0x805986C0`) is "camera
  use", and when it is 0 the check is skipped. `recomp.toml` patches the settings defaults
  (`0x80073BD8`) to 0; since milestone 11 the launcher sets the byte at every start-up (see
  "NamCam2 camera" above).
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
- **Clock**: the game reads the date from `OSGetTime()` (the time base, which SegaBoot sets), so
  the time base starts at the local time (`os::boot_time_base()`). Among other things, the game
  asks for the card reader to be cleaned when a week (168 hours) has passed since the last
  cleaning, a time kept in the settings block (`+0x18`); a clock that restarted at 2000-01-01 made
  that last cleaning always lie in the future, and the question came back at every start-up.
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

### The games' debug messages (milestone 10)

- `DebugPrintf` (`0x8002CE8C`; GP `0x80030760`) has about 570 calls in GP 2: scene changes, the
  start-up checks, the camera, the link, the card reader and printer, memory use. It prints
  through `OSReport` only when byte `0x2A` of the settings block is set, which start-up clears.
- The SDK's `OSPanic`, which reports failed checks (`DVDRead`, `VIConfigure`, `__assert`, ...),
  is an empty function in both games (`OSPanic_Stub`, `0x8002CDEC`; GP `0x800306C0`), so they
  carry on after one.
- `--game-log` (`games/common/game_log.cpp`) replaces both to print `[game]` lines, converted
  from Shift JIS. GP 2's start-up prints about 40 lines (the cabinet's address, the wheel's
  calibration, the sound and card printer set-up); its attract mode and menus print almost
  nothing. GP prints its memory use at each scene change.

## Mario Kart Arcade GP (GGPE01): what milestone 8 found

GP 2 is a sequel on the same engine, and most of what it needed carried over unchanged: the
runtime (Triforce boards, GPU, audio, launcher) has no game-specific code besides the media board
telling the two apart. The game-specific part is `games/mkgp1/`: the symbol list, four addresses
in `recomp.toml` and one patched function.

### Executable (`main.dol`, 4.13 MiB)

- Built with the same Dolphin SDK (17 April 2003 release, OS of 21 May 2004), MusyX and HSD as
  GP 2. decomp-toolkit finds 7,504 functions; `tools/sigmatch.py` names 632 more of them.
- `__start` at `0x80003154`; `_SDA_BASE_` = `0x805BDE20`, `_SDA2_BASE_` = `0x805C14E0`.
- 24 MiB of RAM (arena `0x805D1FA0`-`0x817DE380`).
- Matching functions between the two games by their code (with relocated fields masked, and the
  same size) pairs up 3,367 of them one to one (`tools/crossmatch.py`). That is how most of the
  addresses below were found, and how 1,021 functions and 77 objects got GP 2's names from
  the decomp.

### What it needed

| What | GP 2 | GP | Notes |
|------|------|----|-------|
| SelectThread's idle loop (hook) | `0x8025BA28` | `0x8021327C` | SDK |
| Settings block (backup memory) | `0x805986C0` | `0x804B3B40` | Steering centre at `+0x38` (GP 2: `+0x34`), gas and brake rest positions at `+0x44` / `+0x40` (GP 2: `+0x40` / `+0x3C`) |
| Camera use default (patch) | `0x80073BD8` | `0x800790A0` | Byte `+0x25` of the settings in both; without the patch GP's device check waits at CAMERA |
| LINK wait of 3600 frames (patch) | `0x80084850` | `0x801B0E28` | Identical code |
| Coin set-up / coin settings | `0x80289B54` / `0x80690AA0` | `0x8024DEDC` / `0x80577740` | Free play |
| Restart into the test menu | `0x802878CC` | `0x8024BBC4` | Identical code: 0x100 bytes at `811FFF00`, first word 1 |

- The analog calibration (`clInputJAMMA`: steering centre +/- `0x5000`, pedals `0x800` above rest
  to `0x4000` further) is the same, so the I/O board's scaling fits both.
- The wheel's force feedback uses the same protocol. A race ramps centering up to about `0x2D00`
  and friction to `0x1400` (GP 2: `0x2800` / `0x1E00`), with the same `0x7F00`/`0x8100` roll buzz.
- The card reader and the Mario Card work as in GP 2 (Dolphin's reader already covers GP's
  dispense command without options). GP's cards are 69 bytes.
- The test button reboots into the test menu, and EXIT back into the game, as in GP 2.

### Recompiler fixes found on the way

Both games called the SDK's `_savegpr_N` / `_restgpr_N` (labels inside `__save_gpr` /
`__restore_gpr`) and tail-called functions through `bctr`, which fell back to the interpreter. The
recompiler now gives every call or branch target inside another function a function of its own,
and `ppc::jump` tail-calls a recompiled function when there is one. `OPENGP_LOG_INTERP=1` lists
anything still interpreted; neither game has any in attract mode or a race.

