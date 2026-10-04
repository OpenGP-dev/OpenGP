// SPDX-License-Identifier: GPL-3.0-or-later
//
// Saving host files (settings, save cards) so that a crash or power cut never
// leaves one half written.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace os {

// A path from UTF-8 text (std::filesystem would read a std::string in the
// system's code page on Windows).
inline std::filesystem::path utf8_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Writes `size` bytes to `path` (UTF-8): to a temporary file next to it first, which
// then replaces `path`. Creates the folder if needed.
bool write_file_atomically(const std::string& path, const void* data, std::size_t size);

}  // namespace os
