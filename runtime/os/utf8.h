// SPDX-License-Identifier: GPL-3.0-or-later
//
// Paths and arguments as UTF-8. OpenGP keeps paths, file names and the
// command line as UTF-8 text in std::string everywhere, and converts only at
// the edges with these. On Windows, a std::string given to std::filesystem,
// fopen or a file stream is read in the system's code page, and main()'s argv
// arrives in it: without them, a folder named "José" or "日本" is not found.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace os {

// A path from UTF-8 text.
inline std::filesystem::path utf8_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// UTF-8 text from a path.
inline std::string utf8(const std::filesystem::path& path) {
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

// std::fopen with a UTF-8 path.
std::FILE* fopen_utf8(const std::string& path, const char* mode);

// main()'s arguments as UTF-8 (on Windows, from the wide command line).
std::vector<std::string> utf8_args(int argc, char** argv);

}  // namespace os
