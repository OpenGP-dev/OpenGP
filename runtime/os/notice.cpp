// SPDX-License-Identifier: GPL-3.0-or-later

#include "os/notice.h"

#include <cstdio>
#include <mutex>

namespace os {

namespace {

std::mutex g_mutex;
std::vector<std::string> g_notices;

}  // namespace

void notify_player(const std::string& message) {
  std::fprintf(stderr, "[opengp] %s\n", message.c_str());
  std::lock_guard lock(g_mutex);
  if (g_notices.size() < 16) g_notices.push_back(message);
}

std::vector<std::string> take_player_notices() {
  std::lock_guard lock(g_mutex);
  std::vector<std::string> out;
  out.swap(g_notices);
  return out;
}

}  // namespace os
