# Playing

## Installing

- **From the installer package**: unpack it, put your disc images in its `ISO` folder and run
  `OpenGP` (or drop an image on `OpenGP.exe`). It builds each game you have an image of, once;
  see [packaging.md](packaging.md). The launchers then sit next to it.
- **From source**: see [building.md](building.md) and [game-files.md](game-files.md).

## Starting a game

Each game has its own launcher: `opengp-mkgp1` for Mario Kart Arcade GP and `opengp-mkgp2` for
Mario Kart Arcade GP 2. They work the same way, and everything on this page applies to both.

The games behave like the arcade cabinet: insert a coin and press start; in the menus, steer to
choose and step on the gas pedal to decide. Turn on free play in the menu (Esc), or start the
launcher with `--free-play`, to play without coins ("Step on gas pedal to start").

```sh
build/<preset>/games/mkgp1/opengp-mkgp1 --free-play
build/<preset>/games/mkgp2/opengp-mkgp2 --free-play
```

The first time, the game asks "Do you have Mario Card?". Choose **No**, then **Yes** to make one:
the card keeps your progress (Mario Coins, titles, unlocked characters). See [Mario Cards](#mario-cards).

## The menu

**Esc** (or a gamepad's guide button) opens the menu over the game; Esc again closes it. The game
pauses while it is open and gets no controls (turn off **Pause while this menu is open** in the
Game tab to keep it running).

| Tab | Settings |
|-----|----------|
| Game | The Mario Card you hold; the camera; pausing; free play |
| Video | Fullscreen, aspect ratio, internal resolution, VSync, even frame pacing |
| Sound | Volume |
| Controls | The keys, buttons and axes for each control; force feedback, steering speed, dead zones |
| Profiler | Recording a profile, marking a moment, capturing a frame (see [profiling.md](profiling.md)) |

**F11** (or Alt+Enter) switches between a window and fullscreen, and **F12** saves a screenshot to
`user/screenshots/`.

If the game stutters or draws something wrong, **F10** records a profile of how it runs, **F8**
marks the moment something happened, and **F9** captures a frame that looks wrong; see
[profiling.md](profiling.md) for what they record and how to send them.

### Pausing

**P** (or the Pause key) pauses the game and shows "Paused"; press it again to carry on. The arcade
games have no pause of their own, so OpenGP freezes the whole machine: the game, its timers and
its sound stop where they are, and carry on as if no time had passed. The menu pauses the game
too, and **Pause when the window is unfocused** (Game tab) pauses it while you are in another
window. The pause control can be changed like the others (Controls tab, or `pause` in
`controls.toml`). The game's clock falls behind by the time spent paused, until it next starts.

Pausing is only for playing solo. Once the game has linked to another cabinet (on this computer or
a LAN), the other cabinets need it to keep running, so it doesn't pause again until it restarts:
the pause control does nothing, and the menu opens over the running game.

## Settings

The menu's settings are saved in `user/settings.toml` when the menu closes and when the game quits,
along with the window's size. The file can be edited by hand; command-line options (see
[building.md](building.md#running)) override it for one run without changing it.

### Widescreen

The aspect ratio is **Fit the window** by default: the picture takes the window's shape, from 4:3
up to 32:9 (a window narrower than 4:3 gets black bars above and below). 4:3, 16:9, 16:10 and 21:9
can also be chosen; the window then gets bars at the sides if its shape differs.

In widescreen, the races show more on each side. The game's 2D graphics (the speedometer, the map,
the menus' panels) stay in the middle at their original shape, full-screen backgrounds and fades
are stretched to the edges, and banners that run off the side of the 4:3 screen are continued to
the edge of the wide one.

### Internal resolution

The game renders at its native 640x528 times the internal resolution. **Match the window** (the
default) picks the smallest multiple at least as tall as the window: 2x for 720p, 3x for 1080p,
5x for 4K.

### Even frame pacing

The game runs on its own 60 Hz clock, but it takes more time over some frames than others, and
OpenGP shows each frame as soon as it is drawn. A monitor with a variable refresh rate (FreeSync
or G-Sync) shows each frame as it comes, so frames can be a few milliseconds early or late and
the motion slightly uneven. With **Even frame pacing** (Video tab, off by default; `[video]
even_pacing` in `user/settings.toml`), OpenGP holds each frame until the game's next tick, as a
cabinet's display does, so the frames come evenly spaced. It costs about half a frame (8 ms) of
delay, and matters most in fullscreen with FreeSync or G-Sync.

## Mario Cards

Cards are saved in `user/cards/GGPE01/` (GP) and `user/cards/GGPE02/` (GP 2), one file per card
(`Card 1.bin`, ...). In the menu's Game tab you choose the card you hold: you insert it when the
game asks for your card, and the game takes it back at the end. **New card** gives you a blank one (answer **No** to "Do you have Mario
Card?", then buy a card, and it is saved under that name); **No card** plays without one. A card the
game gives you is yours to hold from then on, so you can keep a card per player. Files can be
renamed (the name is only for the menu) or copied to back them up.

A card saved by an earlier version as `user/tricard_GGPE02.bin` is moved to `Card 1.bin`.

## The camera

The cabinet has a camera (the NamCam2) that takes your photo when you start a game: "Please align
your face within the frame", then the item button. The photo is shown on your character in the
race. OpenGP can use a webcam for it. In the menu's Game tab, **Photos from** is:

- **Off** (the default): the game takes no photos, as a cabinet without its camera.
- **Webcam**: the webcam chosen below it (**Default** is the first one). Its picture is cropped to 4:3 around
  the middle. On macOS, the system asks once whether OpenGP may use the camera.
- **Test pattern**: colour bars, for trying it without a webcam.

The camera is set up when the game starts, so a change takes effect when the game restarts. If
there is no webcam, the game runs without its camera and says so on the console. In
`user/settings.toml` the setting is `[camera] source` (`"off"`, `"webcam"` or `"test"`) with
`webcam` (the webcam's name, `""` for the first), and `--camera off|webcam|test` sets it for one
run.

The webcam is on only while the game uses it: while your photo is taken, for a moment as the game
starts (it checks its camera), and on the test menu's camera page. Its light goes off a few
seconds later. If the webcam can't be opened then (another program is using it, say), the photo
is grey and the console says why.

## Linked cabinets

Up to four cabinets link, so that up to four players race each other in Versus mode, as in an
arcade. Each cabinet is a copy of OpenGP: on the same computer, or on computers on the same local
network. Both games link, each with its own kind (GP 2 with GP 2, GP with GP).

Each cabinet needs its own number, 1 to 4: **Cabinet number** in the menu's Game tab
(`[link] cabinet` in `user/settings.toml`, or `--cabinet N` for one run). **The game's setting**
keeps the number set in the game's test menu, which is 1 unless changed there. The number is read
when the game starts, so a change takes effect when it restarts.

- **On one computer**, give each copy its own user folder as well as its own number, since two
  copies sharing one would overwrite each other's settings and saves:
  `opengp-mkgp2 --user-dir user2 --cabinet 2`. To drive them all at once from one keyboard or
  gamepad, turn on **Use the controls in the background too** (Controls tab) in each.
- **On a local network**, turn on **Link over the local network** on each computer
  (`[link] lan = true`, or `--lan`). The cabinets find each other by broadcasting on the network.
  The first time, Windows asks whether OpenGP may use the network: allow it (on the kind of
  network you are on, which Windows may call public). macOS asks whether OpenGP, or the Terminal
  you start it from, may find devices on your local network: allow that too.
  The link uses ports 28500 to 28539 (TCP and UDP) on each computer; `[link] port` moves them
  (give every cabinet the same).

Start the cabinets, and wait for the later ones to get through their start-up (the attract mode
begins): a cabinet links to the others as it starts. Then start a game on one cabinet. The others show "Waiting for entry!", and players who insert a
coin there (or press the gas pedal, with free play) before the time runs out join the race. The
Game tab says which cabinets this one is linked with, and if another cabinet has the same number.

A linked game can't pause (see [Pausing](#pausing)). If a cabinet stops in the middle of a game,
the others stop too with "Please call an attendant / Link error occurred", as arcade cabinets
do: OpenGP restarts them after a few seconds.

## Discord

While Discord runs on the same computer, your Discord profile shows what you are playing, as
**Playing OpenGP**: the game, and whether you are choosing a character or a course (or having your
photo taken, with the camera on); in a race, the cup, the round and the course, your character, your
place and the lap, with the time since the race started; then the place you finished, with the cup's
trophy if you won. The pictures are the games' own, of the characters and courses. For example:

    Yoshi Cup · Round 1: Yoshi Park 1
    Mario · 3rd · Lap 2/3

OpenGP talks only to the Discord app on your computer, through its local connection, and sends
nothing anywhere else. If Discord starts after the game, your profile shows it within 15 seconds.
To turn it off, untick **Show what you're playing** in the menu's Game tab (`[discord]
rich_presence = false` in `user/settings.toml`). Discord's own Activity Privacy settings can hide
it as well.

## Controls

| Cabinet | Keyboard | Gamepad |
|---------|----------|---------|
| Steering | Left / Right, A / D | Left stick, d-pad |
| Gas | Up, W | Right trigger |
| Brake | Down, S | Left trigger |
| Item button | Space, J | A (bottom face button), right shoulder |
| VS cancel button | Backspace, K | B (right face button), left shoulder |
| Start | Enter, 1 | Start |
| Coin | 5, C | Back |
| Service / test | F1 / F2 | |
| Menu | Esc | Guide |
| Pause | P, Pause | |
| Fullscreen | F11, Alt+Enter | |
| Screenshot | F12 | |
| Record a profile (start/stop) | F10 | |
| Mark the moment in a profile | F8 | |
| Capture a frame | F9 | |

Every connected device works at once. Keys and d-pad steer gradually (a keyboard cannot hold the
wheel half-way); sticks and wheels steer directly.

### Changing them

In the menu's **Controls** tab, choose the keyboard, gamepads or wheels and joysticks, then click
**+** next to a control and press the key or button (or move the axis, the pedal or the wheel)
to add it; click an input to remove it. Changes apply at once and are saved when the menu closes.

They are kept in `user/controls.toml`, which is written with the defaults the first time the game
runs (with a window). It can also be edited by hand (then restart); delete it to get the defaults
back. Each control lists the inputs that drive it:

```toml
[keyboard]
gas = ["Up", "W", "Left Shift"]   # SDL key names

[gamepad]
item = ["a", "x"]                 # SDL gamepad button names

[joystick]                        # wheels and pedals SDL does not know as a gamepad
steer = ["axis0"]
gas = ["-axis2"]                  # a pedal that reads highest when released
brake = ["-axis3"]
```

Axes: `axis2` is the whole axis, `axis2+` and `axis2-` are one half of it, and a leading `-`
turns it around. The file itself lists every name.

Normally only the active window reads the controls. **Use the controls in the background too**
(Controls tab; `[background] input` in the file) makes every OpenGP window read them, active or
not, to drive several copies on one computer at once (see [Linked cabinets](#linked-cabinets)):
gamepads on every system, and the keyboard on Windows. The windows then all do the same.

## Steering wheels and force feedback

A wheel that SDL does not recognise as a gamepad uses the `[joystick]` bindings; set up its
pedals there (their axis numbers differ between wheels). Wheels with force feedback get the
cabinet's effects: a centering spring, friction, and pushes from bumps and crashes. Gamepads
rumble on bumps and crashes. `[force_feedback]` in `controls.toml` sets the strength (0 turns it
off).

## Sound

Sound plays on the system's default output device. The menu's volume (or `--volume PERCENT`)
turns it down, and `--no-audio` turns it off.

The attract mode (the demo that loops before a coin goes in) has sound every time round. GP 2 by
itself plays its first time round without sound; OpenGP turns that off. Its volume is the game's
own ATTRACT setting, in the test menu's SOUND SETTING (8 of 15 by default). The demo race in each
round is silent; whether it is on a cabinet is not known yet.

## Scripted input

For tests and screenshots without a player, `--input` and `--input-file` play controls by frame
number (see `runtime/input/script.h`). This goes from start-up to a race and holds the gas:

```sh
opengp-mkgp2 --headless --free-play --timeout 120 --screenshot-every 600 \
  --input "700 gas=255; +60 gas=255; +60 item; +60 item; +60 item; +60 item; +60 item; +60 item; +60 item; +60 item; 2400-9000 gas=255"
```

Each event is a time (`600`, `+60` after the previous event, or a range `1000-2000`) and what to
hold: `coin`, `start`, `item`, `cancel`, `test`, `service`, `steer=0..255` (128 is straight
ahead), `gas=0..255`, `brake=0..255`. Buttons without a range are held for 10 frames.

## Cleaning the card reader

The game's clock is the computer's. Once a week, at start-up, the game asks for the card reader to
be cleaned ("Please clean the card R/W"): press the **cancel button** (Backspace) to clean it
(OpenGP inserts the cleaning card), or the **item button** (Space) to skip it until the next
start-up.

## The test menu

The game's own settings (the operator's: difficulty, sound in the attract mode, card price,
bookkeeping and so on) are in its **test menu**. Press **F2** (the cabinet's test button) at any
time, or use **Test menu** in the menu's Game tab, or start with `--test-menu`: the game restarts
into it (the window closes and opens again, as the cabinet reboots). In the test menu, **F1** (the
service button) moves to the next item and **F2** chooses it; **EXIT** restarts the game.

Holding **F1** and pressing **F2** during the game opens the I/O test instead (wheel and pedal
calibration and a switch test); hold F1 and press F2 again to leave it.

## If the game freezes

If the game's picture stops for 10 seconds while it is not paused, OpenGP writes where the game's
threads are to `user/hang-report.txt` (and the console). Please send that file with the report:
it shows what the game was waiting for.

