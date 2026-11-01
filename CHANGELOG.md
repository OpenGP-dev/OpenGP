## 1.0.1 (2/11/2026)

- GP 1: fixed objects popping in at the sides of the screen in widescreen
- Fixed flickering surfaces in Yoshi Park’s Trick House
- Fixed some HUD elements and banners looking stretched in widescreen
- Fixed odd gradient bars at the sides of menus in widescreen
Fixed some screen effects looking squeezed in widescreen
macOS: fixed the rainbow fade effect in GP 2
Fixed some highlight effects showing as solid white
Fixed a small periodic stutter on 60 and 120 Hz displays
Improved stability of linked play
Fixed audio drifting out of sync after changing the audio device
Fixed occasional audio stutter
Fixed a brief freeze when a link connection was refused
Linked cabinets on mismatched versions now tell you why they can’t connect
Fixed steering stuck at full lock when extra USB devices (pedals, handbrakes) were connected
Fixed pedals reading as half-pressed
Fixed pedals binding to the wrong axis half
Improved wheel and pedal precision
Fixed menu keys carrying over into the game
Fixed a bug where binding certain keys broke the controls file
Key bindings now show the correct key labels on non-US keyboards
Added an alternative gamepad button for opening the menu
Menu sliders now stay within their range
“Restore defaults” now asks for confirmation
Fixed Mario Card progress being lost when saving failed, and the game now tells you
Fixed card and settings saves failing on Windows with OneDrive or antivirus running
A bad value in the settings file no longer resets everything
Settings and controls are now saved when you close the menu
Fixed the second-cabinet command not finding the game
Fixed the game not finding its image when launched from another folder
Fixed crashes and install failures when the folder path contains accented or non-Latin characters
Fixed the game ignoring or crashing on ISO files with non-ASCII names
Start-up errors are now shown in a message box
Fixed the default window size on high-DPI displays
Removed the debug spam from the console
The webcam no longer switches on at start-up
Fixed the first photo coming out grey or dark
Linux: fixed the webcam staying on after a game restart
Added a first-run hint for inserting a coin and opening the menu
Improved compiler download feedback and recovery from interrupted installs
Clearer messages for unsupported or compressed disc images
Clearer errors for unwritable or zipped folders
The menu no longer shows your full folder path

### Known issues

- Races need most of a CPU core: a PC busy with something else (another game) can drop below
  60 fps.
