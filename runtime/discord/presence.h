// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discord Rich Presence: what Discord shows on the player's profile under
// "Playing <the application's name>". The application (its name and its art
// assets) is set up in Discord's developer portal and named by its ID.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "discord/ipc.h"

namespace discord {

struct Activity {
  std::string details;  // the first line
  std::string state;    // the second line
  std::int64_t start = 0;  // shows the time since then (Unix seconds); 0 for none
  // Art assets by their keys in the application, with their tooltips.
  std::string large_image, large_text;
  std::string small_image, small_text;

  bool operator==(const Activity&) const = default;
};

// The SET_ACTIVITY command (no activity clears it). `pid` is the process the
// activity belongs to: Discord clears it when the process ends.
std::string set_activity_json(const std::optional<Activity>& activity, long pid, std::string_view nonce);

// The handshake for an application.
std::string handshake_json(std::string_view application_id);

// Keeps an activity on Discord, on a thread of its own: connects when Discord
// is running (and again if it restarts), and sends changes once every 5
// seconds at most (Discord allows 5 in 20 seconds), so a quick change waits a
// little and only the latest is sent. Without Discord, it does nothing.
class Presence {
 public:
  struct Options {
    std::function<std::unique_ptr<Transport>()> connect = connect_local;
    int retry_ms = 15000;        // between attempts to connect
    int min_interval_ms = 5000;  // between updates
    bool log = true;             // say on the console when it connects
  };

  explicit Presence(std::string application_id) : Presence(std::move(application_id), Options{}) {}
  Presence(std::string application_id, Options options);
  ~Presence();
  Presence(const Presence&) = delete;
  Presence& operator=(const Presence&) = delete;

  // The activity to show; none clears it.
  void set(std::optional<Activity> activity);

  // Whether Discord is connected (for tests and the menu).
  bool connected() const { return connected_; }

 private:
  void run();
  bool handshake(Transport& t);

  const std::string id_;
  const Options options_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<Activity> wanted_;
  bool stop_ = false;
  std::atomic<bool> connected_{false};
  std::thread thread_;
};

}  // namespace discord
