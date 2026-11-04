// SPDX-License-Identifier: GPL-3.0-or-later
//
// Running other programs (the compiler, the linker, curl and tar) and
// collecting what they print.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace installer {

struct ProcessResult {
  int exit_code = -1;  // -1 if it could not be started
  std::string output;  // stdout and stderr together
};

// Runs `args[0]` (a path, or a name looked up on PATH) with the rest as its
// arguments (UTF-8), in the folder `cwd` (UTF-8; "" for this process's), and
// waits for it. It gets no standard input, and inherits nothing else.
ProcessResult run_process(const std::vector<std::string>& args, const std::string& cwd = {});

// Runs the commands, up to `jobs` at a time. Calls `done(index, result)` (from
// the calling thread's point of view, one call at a time) as each finishes;
// if it returns false, no further commands are started. Returns true if all
// of them ran and succeeded.
bool run_parallel(const std::vector<std::vector<std::string>>& commands, unsigned jobs,
                  const std::function<bool(std::size_t index, const ProcessResult& result)>& done);

}  // namespace installer
