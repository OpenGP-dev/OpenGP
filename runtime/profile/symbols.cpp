// SPDX-License-Identifier: GPL-3.0-or-later

#include "profile/symbols.h"

#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#else
#include <dlfcn.h>
#endif

namespace profile {

using namespace gekko;

#ifdef _WIN32

HostSymbol host_symbol(u64 address) {
  static std::mutex mutex;
  static bool initialised = false;
  static std::unordered_map<u64, std::string> modules;  // by base address
  std::lock_guard lock(mutex);
  const HANDLE process = GetCurrentProcess();
  if (!initialised) {
    // Symbols load as each module is first looked at (the game's, from its
    // .pdb next to it, takes a moment).
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    initialised = true;
  }
  HostSymbol out;
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
  auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
  info->SizeOfStruct = sizeof(SYMBOL_INFO);
  info->MaxNameLen = 511;
  DWORD64 displacement = 0;
  if (SymFromAddr(process, address, &displacement, info)) {
    out.name.assign(info->Name, info->NameLen);
    out.offset = displacement;
  }
  const u64 base = SymGetModuleBase64(process, address);
  auto [it, added] = modules.try_emplace(base);
  if (added && base) {
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, base, &module)) it->second = module.ModuleName;
  }
  out.module = it->second;
  if (out.name.empty() && base) out.offset = address - base;
  return out;
}

#else

HostSymbol host_symbol(u64 address) {
  HostSymbol out;
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(address), &info)) {
    if (info.dli_sname) {
      out.name = info.dli_sname;
      out.offset = address - reinterpret_cast<u64>(info.dli_saddr);
    } else {
      out.offset = address - reinterpret_cast<u64>(info.dli_fbase);
    }
    if (info.dli_fname) {
      const std::string_view path = info.dli_fname;
      out.module = std::string(path.substr(path.find_last_of('/') + 1));
    }
  }
  return out;
}

#endif

std::string readable_symbol(const std::string& name) {
  std::string_view n = name;
  if (n.rfind("weak.", 0) == 0) {
    const std::size_t def = n.find(".default.");
    if (def != std::string_view::npos) {
      auto bare = [](std::string_view s) {
        while (!s.empty() && s.front() == '?') s.remove_prefix(1);
        if (s.rfind("__imp__", 0) == 0) s.remove_prefix(7);
        return s.substr(0, s.find("guest"));
      };
      return std::string(bare(n.substr(5, def - 5))) + " (" + std::string(bare(n.substr(def + 9))) + ")";
    }
  }
  if (n.rfind("guest::", 0) == 0) n.remove_prefix(7);
  if (n.rfind("__imp__", 0) == 0) n.remove_prefix(7);
  return std::string(n);
}

bool is_guest_symbol(const std::string& name) {
  return name.find("__imp__") != std::string::npos || name.rfind("guest::", 0) == 0;
}

}  // namespace profile
