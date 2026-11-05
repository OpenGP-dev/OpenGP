## 1.0.4 (Unreleased)

- GP 2: the brake goes back through this play's menu screens to change a class, character, kart, cup, course or round. **Brake goes back** is on by default in the Game tab and can be turned off.
- Going back skips the photo screen and stops working once the gas confirms a choice on the current screen. Menu history resets when a race starts. This feature is disabled in linked play and does not apply to GP 1.

## 1.0.3 (7/11/2026)

- Fixed the periodic freezes in linked races: the cabinets now keep in step (a cabinet runs a fraction of a percent faster or slower to follow the one that leads the race), and one that falls a frame behind catches up without freezing the race

## 1.0.2

- The command-line installer is now called `opengp-cli`, so it is no longer mistaken for the way to install OpenGP 
- The menu no longer shows your full folder path: an **Open folder** button opens the card or controls folder instead
- Plainer names and help text in the menu's settings, These now match the settings text from the launcher
- Aspect ratio and resolution choices now name their sizes (16:9, 1080p, 4K and so on)
- Gamepads without a Guide button (or where Steam takes it) open the menu with **Back** and **Start** held together
- Key bindings now show the correct key labels on non-US keyboards
- Fixed the default window size on high-DPI displays
- Removed debug spam from the console (use `--verbose` to restore pre 1.0.2 behaviour)
- Added a first-run hint for inserting a coin and opening the menu
- Improved wheel and pedal precision
- Fixed pedals reading as half-pressed until they were first moved
- Fixed pedals binding to the wrong axis half when bound before they were first moved
- The webcam no longer switches on at start-up: the game's camera check gets a picture without it
- Fixed the first photo coming out darker than intended
- Fixed audio drifting out of sync after changing the audio device
- Fixed occasional audio stutter
- Fixed a small periodic stutter on 60 and 120 Hz displays

### Known issues

- Races need most of a CPU core: a PC busy with something else (another game) can drop below
  60 fps.
- Linked races can still freeze for a moment now and then; this is being fixed for 1.0.3.
- 1.0.2 cabinets link with 1.0.1 ones, but not with 1.0.0, run 1.0.1 or newer on every cabinet.

## 1.0.1 (2/11/2026)

- A new in-game menu in the OpenGP launcher's look: a pause card, with the settings in a dialog
- GP 1: fixed objects popping in at the sides of the screen in widescreen
- Fewer dropped frames on Windows: the game's timed events now arrive on time
- Less GPU work at high internal resolutions, for the same picture
- Fixed the picture staying blank or white after the GPU briefly ran out of memory
- Fixed a possible freeze in effects that read the picture back
- Fixed crashes and freezes caused by damaged disc images or bad game data
- Linked play: less lag between computers, and cabinets are harder to disrupt by mistake
- Linked cabinets only connect over the local network (virtual LANs such as Radmin VPN and Hamachi still work)
- Fixed a brief freeze when a link connection was refused
- Linked cabinets on mismatched versions now tell you why they can't connect
- Fixed steering stuck at full lock when extra USB devices (pedals, handbrakes) were connected
- Fixed inputs a device doesn't have reading as half-pressed
- Steering wheels get fewer force-feedback updates: only forces that change are sent
- Fixed menu keys carrying over into the game
- Fixed a bug where binding certain keys broke the controls file
- Menu sliders now stay within their range
- "Restore defaults" now asks for confirmation
- Fixed Mario Card progress being lost when saving failed, and the game now tells you
- Fixed card and settings saves failing on Windows with OneDrive or antivirus running
- A bad value in the settings file no longer resets everything
- Settings and controls are now saved when you close the menu
- A second copy of a game can no longer share a user folder with one that is running
- Fixed the second-cabinet command not finding the game
- Fixed the game not finding its image when launched from another folder
- Fixed crashes and install failures when the folder path contains accented or non-Latin characters
- Fixed the game ignoring or crashing on ISO files with non-ASCII names
- Start-up errors are now shown in a message box
- Crashes now write a crash report (on Windows, the game tells you where it is)
- Linux: fixed the webcam staying on after a game restart
- Linux: the installer finds GCC 12 and newer Clang versions, so Ubuntu 22.04 works
- Improved compiler download feedback and recovery from interrupted installs
- Clearer messages for unsupported or compressed disc images
- Clearer errors for unwritable or zipped folders

### Known issues

- Races need most of a CPU core: a PC busy with something else (another game) can drop below
  60 fps.

