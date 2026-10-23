// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/card_folder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "os/atomic_file.h"

namespace triforce {

namespace fs = std::filesystem;

CardFolder::CardFolder(std::string dir) : dir_(std::move(dir)) {}

bool CardFolder::valid_name(const std::string& name) {
  // Everywhere: one file in the folder.
  if (name.empty() || name.size() > 240 || name == "." || name == "..") return false;
  for (const char c : name)
    if (static_cast<unsigned char>(c) < 0x20 || c == '/') return false;
#ifdef _WIN32
  // Windows' own rules: other systems allow these names, and their cards keep working.
  if (name.back() == '.' || name.back() == ' ') return false;
  if (name.find_first_of("\\:*?\"<>|") != std::string::npos) return false;
  // Windows' device names, with or without an extension.
  std::string base = name.substr(0, name.find('.'));
  while (!base.empty() && base.back() == ' ') base.pop_back();
  for (char& c : base) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const char* device : {"CON", "PRN", "AUX", "NUL"})
    if (base == device) return false;
  if (base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && base[3] >= '0' && base[3] <= '9')
    return false;
#endif
  return true;
}

bool CardFolder::exists(const std::string& name) const {
  std::error_code ec;
  return valid_name(name) && fs::is_regular_file(os::utf8_path(path(name)), ec);
}

std::vector<std::string> CardFolder::list() const {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(os::utf8_path(dir_), ec)) {
    if (entry.path().extension() != ".bin" || !entry.is_regular_file(ec)) continue;
    const auto stem = entry.path().stem().u8string();
    std::string name(stem.begin(), stem.end());
    if (valid_name(name)) names.push_back(std::move(name));  // only cards it can hold
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
  if (!name.empty() && !valid_name(name)) {
    std::fprintf(stderr, "[card] \"%s\" cannot be a card's name: holding no card\n", name.c_str());
    name.clear();
  }
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
