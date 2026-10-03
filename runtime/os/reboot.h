// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rebooting the game (OSResetSystem). On the hardware, a restart reloads the
// game, and the SDK hands the new boot a reset code and a region of memory
// kept from before (OSSetSaveRegion): Triforce games use it to boot into
// their test menu, and back out of it. OpenGP restarts the whole process
// instead: the launcher saves the reboot to a file and starts itself again
// (restart_process()), and the new process gives the reset code and memory
// back to the game when its OSInit asks (__OSGetExecParams, see
// games/common/sdk_hle.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace os {

using namespace gekko;

struct Reboot {
  bool restart = true;  // false: a hot reset or shutdown, so a fresh boot with nothing kept
  u32 reset_code = 0;
  u32 region_start = 0, region_end = 0;  // guest addresses of the memory kept
  std::vector<u8> region;                // its contents
};

bool save_reboot(const std::string& path, const Reboot& reboot);
bool load_reboot(const std::string& path, Reboot* reboot);

// What happens when the game reboots: set by the launcher. It is called on
// the guest thread, which then waits forever (the process is restarted).
extern std::function<void(const Reboot&)> g_reboot_handler;
[[noreturn]] void reboot(const Reboot& reboot);

// The reboot this boot comes from, or nullptr after power-on.
extern const Reboot* g_previous_boot;

// Replaces this process with a new run of the same executable (`argv0` is
// how it was started) with `args` (argv[1...]). Standard input and output
// are passed on.
[[noreturn]] void restart_process(const std::string& argv0, const std::vector<std::string>& args);

}  // namespace os
