// SPDX-License-Identifier: GPL-3.0-or-later
//
// Folders for the tests' files, under the system's temp folder in one folder
// per test process: two test runs at once (two builds' ctest) never share a
// file. The process's folder is removed when it exits.
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace opengp_test {

inline const std::filesystem::path& run_dir() {
  struct Run {
    std::filesystem::path dir;
    Run() {
#ifdef _WIN32
      const long pid = _getpid();
#else
      const long pid = static_cast<long>(::getpid());
#endif
      dir = std::filesystem::temp_directory_path() / ("opengp_tests_" + std::to_string(pid));
    }
    ~Run() {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  };
  static const Run run;
  return run.dir;
}

// A fresh, empty folder `name` for a test's files.
inline std::filesystem::path temp_dir(const std::string& name) {
  const std::filesystem::path dir = run_dir() / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

}  // namespace opengp_test
