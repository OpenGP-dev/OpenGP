# Playing

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
keeps running behind it but gets no controls while it is open.

| Tab | Settings |
|-----|----------|
| Game | The Mario Card you hold; free play |
| Video | Fullscreen, aspect ratio, internal resolution, VSync |
| Sound | Volume |
| Controls | The keys, buttons and axes for each control; force feedback, steering speed, dead zones |

**F11** (or Alt+Enter) switches between a window and fullscreen, and **F12** saves a screenshot to
`user/screenshots/`.

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

## Mario Cards

Cards are saved in `user/cards/GGPE01/` (GP) and `user/cards/GGPE02/` (GP 2), one file per card
(`Card 1.bin`, ...). In the menu's Game tab you choose the card you hold: you insert it when the
game asks for your card, and the game takes it back at the end. **New card** gives you a blank one (answer **No** to "Do you have Mario
Card?", then buy a card, and it is saved under that name); **No card** plays without one. A card the
game gives you is yours to hold from then on, so you can keep a card per player. Files can be
renamed (the name is only for the menu) or copied to back them up.

A card saved by an earlier version as `user/tricard_GGPE02.bin` is moved to `Card 1.bin`.

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
| Fullscreen | F11, Alt+Enter | |
| Screenshot | F12 | |

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

## Steering wheels and force feedback

A wheel that SDL does not recognise as a gamepad uses the `[joystick]` bindings; set up its
pedals there (their axis numbers differ between wheels). Wheels with force feedback get the
cabinet's effects: a centering spring, friction, and pushes from bumps and crashes. Gamepads
rumble on bumps and crashes. `[force_feedback]` in `controls.toml` sets the strength (0 turns it
off).

## Sound

Sound plays on the system's default output device. The menu's volume (or `--volume PERCENT`)
turns it down, and `--no-audio` turns it off. The attract mode (the demo that loops before a coin goes in) is
silent, which looks like the game's own setting: the sound starts with the first coin.

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

## Not done yet

- The NamCam2 camera is disabled, and cabinets cannot be linked.

