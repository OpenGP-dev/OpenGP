# Performance information

Open the in-game settings and choose **Performance** to see live frame times,
frame drops and hitches, computer CPU usage, GPU texture usage, audio queue
health and the current scene. The graph holds the last 120 active frames.

Press **F7** to show or hide the performance overlay while playing, or use the
**Performance overlay** switch on that page. The shortcut can be rebound to a
keyboard key, gamepad button or pedal axis in **Controls > Performance**
(`profile_overlay` in `user/controls.toml`). Controllers have no default binding.
Use `--profile-overlay` to show it from startup.

These are live measurements held in memory. This release does not include
profile recording, stack sampling, event logs, marks or profiler frame captures.
The private development profiler retains those tools. Ordinary screenshots and
crash reports are unchanged.

Frame times are wall-clock measurements. Busy time subtracts idle and renderer
waits and is an estimate. GPU texture memory is estimated from game texture
dimensions and mip levels, not total GPU memory. Hitches and dropped retraces
exclude paused/menu intervals and the first frame after resuming. CPU use is
for the whole computer and may be unavailable on some systems.
