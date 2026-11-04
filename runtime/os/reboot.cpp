// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/reboot.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include "os/atomic_file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace os {

namespace {

constexpr char kMagic[8] = {'O', 'G', 'P', 'R', 'B', 'T', '0', '1'};

void put32(std::vector<u8>& out, u32 v) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<u8>(v >> shift));
}

u32 get32(const u8* p) { return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24); }

}  // namespace

std::function<void(const Reboot&)> g_reboot_handler;
const Reboot* g_previous_boot = nullptr;

bool save_reboot(const std::string& path, const Reboot& r) {
  std::vector<u8> data(kMagic, kMagic + sizeof(kMagic));
  put32(data, r.restart ? 1 : 0);
  put32(data, r.reset_code);
  put32(data, r.region_start);
  put32(data, r.region_end);
  put32(data, static_cast<u32>(r.region.size()));
  data.insert(data.end(), r.region.begin(), r.region.end());
  return write_file_atomically(path, data.data(), data.size());
}

bool load_reboot(const std::string& path, Reboot* r) {
  std::ifstream f(utf8_path(path), std::ios::binary);
  if (!f) return false;
  const std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  constexpr std::size_t kHeader = sizeof(kMagic) + 5 * 4;
  if (data.size() < kHeader || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) return false;
  const u8* p = data.data() + sizeof(kMagic);
  const u32 size = get32(p + 16);
  if (data.size() != kHeader + size) return false;
  r->restart = get32(p) != 0;
  r->reset_code = get32(p + 4);
  r->region_start = get32(p + 8);
  r->region_end = get32(p + 12);
  r->region.assign(data.begin() + kHeader, data.end());
  return true;
}

void reboot(const Reboot& r) {
  std::fprintf(stderr, "[os] the game reboots (%s, reset code %08X, keeping %zu bytes at %08X", r.restart ? "restart" : "reset",
               r.reset_code, r.region.size(), r.region_start);
  for (std::size_t i = 0; i < r.region.size() && i < 8; ++i) std::fprintf(stderr, "%s%02X", i ? "" : ": ", r.region[i]);
  std::fprintf(stderr, ")\n");
  if (g_reboot_handler) g_reboot_handler(r);
  // The launcher restarts the process; this guest thread never runs again.
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

void restart_process([[maybe_unused]] const std::string& argv0, const std::vector<std::string>& args) {
  std::fflush(stdout);
  std::fflush(stderr);
#ifdef _WIN32
  auto widen = [](const std::string& s) {
    std::wstring w(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), static_cast<int>(w.size()));
    w.pop_back();  // the terminator
    return w;
  };
  // Quoting as the C runtime parses it: backslashes before a quote are doubled.
  auto quote = [](const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
      if (c == L'\\') {
        ++backslashes;
        continue;
      }
      out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
      backslashes = 0;
      out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
  };
  wchar_t exe[MAX_PATH * 4];
  const DWORD length = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  std::wstring command = quote(std::wstring(exe, length));
  for (const std::string& arg : args) command += L" " + quote(widen(arg));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "[os] could not restart (error %lu)\n", GetLastError());
    std::_Exit(1);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  std::_Exit(0);
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(argv0.c_str()));
  for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  // The new program gets nothing of this one's open files (the webcam, the
  // game's sockets, MEM1's memory file) beyond the console.
  const long max_fd = std::min(sysconf(_SC_OPEN_MAX), 65536L);
  for (int fd = 3; fd < static_cast<int>(max_fd > 0 ? max_fd : 1024); ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
#ifdef __linux__
  execv("/proc/self/exe", argv.data());
#endif
  execvp(argv0.c_str(), argv.data());
  std::perror("[os] could not restart");
  std::_Exit(1);
#endif
}

}  // namespace os
