// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/thread_priority.h"

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#else
#include <pthread.h>

#include <string>
#endif

namespace os {

void set_interactive_thread() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void set_thread_name(const char* name) {
#ifdef __APPLE__
  pthread_setname_np(name);
#elif defined(_WIN32)
  // SetThreadDescription is Windows 10 1607's: looked up, so that older
  // headers and systems do without it.
  using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto set = reinterpret_cast<SetDescription>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (!set) return;
  std::wstring wide;
  for (const char* p = name; *p; ++p) wide += static_cast<wchar_t>(static_cast<unsigned char>(*p));
  set(GetCurrentThread(), wide.c_str());
#else
  pthread_setname_np(pthread_self(), std::string(name).substr(0, 15).c_str());  // 15 characters at most
#endif
}

}  // namespace os
