// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/paths.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "os/atomic_file.h"

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
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

bool lock_user_dir(const fs::path& user_dir, const std::string& game_id) {
  std::error_code ec;
  fs::create_directories(user_dir, ec);
  const fs::path path = user_dir / (".lock-" + game_id);
#ifdef _WIN32
  // Shared, so that a virus scanner or backup tool opening it does not look
  // like another copy; the lock is a byte-range lock on it.
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return true;  // cannot tell: carry on
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return true;  // cannot tell: carry on
#endif
  for (int attempt = 0; attempt < 30; ++attempt) {
#ifdef _WIN32
    OVERLAPPED at{};
    if (LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at)) return true;  // held until exit
    if (GetLastError() != ERROR_LOCK_VIOLATION) return true;  // cannot tell: carry on
#else
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;  // held until exit
    if (errno != EWOULDBLOCK) return true;  // a file system without locks: carry on
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#ifdef _WIN32
  CloseHandle(h);
#else
  ::close(fd);
#endif
  return false;
}

fs::path home_dir() {
#ifdef OPENGP_PACKAGE
  return executable_dir();
#else
  return os::utf8_path(OPENGP_SOURCE_DIR);
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
  const std::string path = os::utf8(images_file(user_dir));
  if (!os::write_file_atomically(path, text.data(), text.size())) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  return true;
}

}  // namespace app
