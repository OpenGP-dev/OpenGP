// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/paths.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "os/atomic_file.h"

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace app {

namespace fs = std::filesystem;

namespace {

fs::path images_file(const fs::path& user_dir) { return user_dir / "images.toml"; }

toml::table read_images(const fs::path& user_dir) {
  std::ifstream f(images_file(user_dir), std::ios::binary);
  if (!f) return {};
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  toml::parse_result parsed = toml::parse(text);
  return parsed ? std::move(parsed).table() : toml::table{};
}

}  // namespace

fs::path executable_dir() {
#ifdef _WIN32
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return fs::current_path();
    if (n < buf.size()) return fs::path(std::wstring(buf.data(), n)).parent_path();
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size + 1);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return fs::current_path();
  std::error_code ec;
  const fs::path p = fs::canonical(buf.data(), ec);
  return ec ? fs::path(buf.data()).parent_path() : p.parent_path();
#else
  std::error_code ec;
  const fs::path p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::current_path() : p.parent_path();
#endif
}

fs::path home_dir() {
#ifdef OPENGP_PACKAGE
  return executable_dir();
#else
  return fs::path(OPENGP_SOURCE_DIR);
#endif
}

std::string remembered_image(const fs::path& user_dir, const std::string& game_id) {
  const toml::table images = read_images(user_dir);
  return images[game_id].value_or(std::string());
}

bool remember_image(const fs::path& user_dir, const std::string& game_id, const std::string& image,
                    std::string* error) {
  toml::table images = read_images(user_dir);
  images.insert_or_assign(game_id, image);
  std::ostringstream out;
  out << "# The disc image each game was installed from (written by the installer).\n" << images << "\n";
  const std::string text = out.str();
  const std::u8string u8 = images_file(user_dir).u8string();
  const std::string path(u8.begin(), u8.end());
  if (!os::write_file_atomically(path, text.data(), text.size())) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace app
