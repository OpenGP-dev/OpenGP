# Security

## Supported versions

Only the latest release gets security fixes.

## Reporting a vulnerability

Please report security problems privately, through GitHub's **Report a vulnerability** button on
the repository's Security tab, rather than in a public issue. Say what is affected, how to
reproduce it, and what an attacker could do. You will get an answer within a week.

## What counts

OpenGP takes input from outside in these places:

- **Linked cabinets** (`runtime/triforce/network.cpp`): UDP beacons and TCP connections on the local
  network, and the games' own link protocol.
- **Disc images** and the files in `user/` (settings, controls, Mario Cards, `reboot.bin`).
- **Discord** (`runtime/discord/`): the local connection to the Discord app.
- **The installer** (`tools/installer/`): the compiler download on Windows and the processes it
  runs.

A way to crash OpenGP, run code, read or write files outside `user/`, or reach another computer
through any of these is a vulnerability.

Linked cabinets work like the arcade machines they emulate: cabinets on the same local network
trust each other, with no passwords or encryption. Only turn on **Link over the local network**
on a network you trust. That linked games can be disrupted by another computer on the same network
is a known limit, not a vulnerability; a way to do more than disrupt the race is one.
