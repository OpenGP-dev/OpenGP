// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace os {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace
#endif

std::FILE* fopen_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
  return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
  int count = 0;
  if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
    std::vector<std::string> args;
    for (int i = 0; i < count; ++i) args.push_back(narrow(wide[i]));
    LocalFree(wide);
    if (count == argc) return args;  // else argv is not this process's command line (a test)
  }
#endif
  return std::vector<std::string>(argv, argv + argc);
}

}  // namespace os
