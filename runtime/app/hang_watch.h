// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hang reports. When the game stops finishing frames while it is not paused,
// OpenGP writes where its threads are (user/hang-report.txt, and the
// console), so that a hang that happens now and then can still be found.
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "gekko/types.h"

namespace opengp {
class Machine;
}

namespace app {

// Function names by address (the game's symbol file).
using FunctionNames = std::map<gekko::u32, std::string>;

// What the guest threads are doing: the registers of the one holding the CPU,
// and each OSThread's state and call stack (the return addresses on its
// stack's back chain).
std::string describe_threads(opengp::Machine& machine, const FunctionNames& names);

// Starts a thread that writes a hang report to `path` once the game has not
// finished a frame for `seconds` while running (30 before its first frame),
// and again after it recovers and hangs again: the first report of a run
// replaces the file, later ones are added to it. Each starts with `header`
// (what is running) and the time. `on_report` gets each report too, if set.
// `machine` and `names` must outlive it.
void start_hang_watch(opengp::Machine& machine, const FunctionNames& names, std::filesystem::path path,
                      std::string header, int seconds = 10,
                      std::function<void(const std::string& report)> on_report = {});

}  // namespace app
