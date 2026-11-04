// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/atomic_file.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace os {

namespace {

namespace fs = std::filesystem;

std::string describe(int code) { return std::system_category().message(code); }

// Writes all of `data` to a new file at `path` and makes sure it is on the
// disk before returning.
bool write_and_sync(const fs::path& path, const void* data, std::size_t size, std::string* error) {
#ifdef _WIN32
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *error = describe(static_cast<int>(GetLastError()));
    return false;
  }
  const char* p = static_cast<const char*>(data);
  bool ok = true;
  for (std::size_t done = 0; ok && done < size;) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1u << 30));
    ok = WriteFile(h, p + done, chunk, &written, nullptr) && written > 0;
    done += written;
  }
  ok = ok && FlushFileBuffers(h);
  if (!ok) *error = describe(static_cast<int>(GetLastError()));
  CloseHandle(h);
  return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    *error = describe(errno);
    return false;
  }
  const char* p = static_cast<const char*>(data);
  bool ok = true;
  for (std::size_t done = 0; ok && done < size;) {
    const ssize_t w = ::write(fd, p + done, size - done);
    if (w < 0 && errno == EINTR) continue;
    ok = w > 0;
    if (ok) done += static_cast<std::size_t>(w);
  }
#ifdef __APPLE__
  // fsync only reaches the drive's cache on macOS.
  ok = ok && (::fcntl(fd, F_FULLFSYNC) == 0 || ::fsync(fd) == 0);
#else
  ok = ok && ::fsync(fd) == 0;
#endif
  if (!ok) *error = describe(errno);
  ::close(fd);
  return ok;
#endif
}

// Replaces `target` with `temp` in one step, retrying for a moment while
// another program (an antivirus, a backup or sync tool, the search indexer)
// has the target open, which makes Windows refuse.
bool replace(const fs::path& temp, const fs::path& target, std::string* error) {
  for (int attempt = 0;; ++attempt) {
#ifdef _WIN32
    if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    const DWORD code = GetLastError();
    const bool busy = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
    if (!busy || attempt >= 10) {
      *error = describe(static_cast<int>(code));
      return false;
    }
#else
    if (::rename(temp.c_str(), target.c_str()) == 0) {
      // The rename itself reaches the disk with the folder's own sync.
      const fs::path dir = target.has_parent_path() ? target.parent_path() : fs::path(".");
      if (const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
        ::fsync(fd);
        ::close(fd);
      }
      return true;
    }
    const int code = errno;
    if (code != EBUSY || attempt >= 10) {
      *error = describe(code);
      return false;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
  }
}

}  // namespace

bool write_file_atomically(const std::string& path, const void* data, std::size_t size, std::string* error) {
  std::string why;
  const fs::path target = utf8_path(path);
  std::error_code ec, folder_ec;
  if (target.has_parent_path()) fs::create_directories(target.parent_path(), folder_ec);
  fs::path temp = target;
  temp += ".tmp";
  if (write_and_sync(temp, data, size, &why) && replace(temp, target, &why)) return true;
  fs::remove(temp, ec);
  // A folder that could not be made is the reason (permission denied, say),
  // not the missing folder the write then found.
  if (folder_ec) why = folder_ec.message();
  if (error) *error = "cannot write " + path + ": " + why;
  return false;
}

}  // namespace os
