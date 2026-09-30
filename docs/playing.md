# Playing

## Starting a game

Mario Kart Arcade GP 2 behaves like the arcade cabinet: insert a coin and press start; in the menus,
steer to choose and step on the gas pedal to decide. Start the launcher with `--free-play` to play
without coins ("Step on gas pedal to start").

```sh
build/<preset>/games/mkgp2/opengp-mkgp2 --free-play
```

The first time, the game asks "Do you have Mario Card?". Choose **No**, then **Yes** to make one:
the card is saved in `user/tricard_GGPE02.bin` and keeps your progress (Mario Coins, titles,
unlocked characters).

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
| Screenshot | F12 | |

Every connected device works at once. Keys and d-pad steer gradually (a keyboard cannot hold the
wheel half-way); sticks and wheels steer directly.

### Changing them

The controls are read from `user/controls.toml`, which is written with the defaults the first time
the game runs (with a window). Edit it and restart; delete it to get the defaults back. Each
control lists the inputs that drive it:

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

## Not done yet

- Sound (milestone 6).
- The game's own test menu: the test switch is not wired up correctly yet (it starts a game
  instead), so the game's settings cannot be changed.
- The NamCam2 camera is disabled, and cabinets cannot be linked.
