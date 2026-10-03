// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/card_folder.h"

#include <algorithm>
#include <filesystem>

#include "os/atomic_file.h"

namespace triforce {

namespace fs = std::filesystem;

CardFolder::CardFolder(std::string dir) : dir_(std::move(dir)) {}

bool CardFolder::exists(const std::string& name) const {
  std::error_code ec;
  return !name.empty() && fs::is_regular_file(os::utf8_path(path(name)), ec);
}

std::vector<std::string> CardFolder::list() const {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(os::utf8_path(dir_), ec)) {
    if (entry.path().extension() != ".bin" || !entry.is_regular_file(ec)) continue;
    const auto stem = entry.path().stem().u8string();
    names.emplace_back(stem.begin(), stem.end());
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string CardFolder::unused_name() const {
  for (int n = 1;; ++n) {
    std::string name = "Card " + std::to_string(n);
    if (!exists(name)) return name;
  }
}

std::string CardFolder::held() const {
  std::lock_guard lock(mutex_);
  return held_;
}

void CardFolder::hold(std::string name) {
  std::lock_guard lock(mutex_);
  held_ = std::move(name);
}

bool CardFolder::inserted() const {
  std::lock_guard lock(mutex_);
  return inserted_;
}

void CardFolder::set_inserted(bool inserted) {
  std::lock_guard lock(mutex_);
  inserted_ = inserted;
}

}  // namespace triforce
