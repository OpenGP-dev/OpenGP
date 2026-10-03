// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/atomic_file.h"

#include <filesystem>
#include <fstream>

namespace os {

bool write_file_atomically(const std::string& path, const void* data, std::size_t size) {
  namespace fs = std::filesystem;
  const fs::path target = utf8_path(path);
  std::error_code ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
  fs::path temp = target;
  temp += ".tmp";
  {
    std::ofstream f(temp, std::ios::binary | std::ios::trunc);
    if (!f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size)) || !f.flush()) return false;
  }
  // rename() replaces the target in one step (MoveFileEx on Windows).
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace os
