// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discord Rich Presence for the games: what the player is doing (the game's
// status, app/game_status.h) shown on their Discord profile, through OpenGP's
// Discord application. Only while Discord runs on the same machine; nothing
// is sent anywhere else.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app/game_status.h"
#include "discord/presence.h"

namespace app {

// OpenGP's application in Discord's developer portal: its name ("OpenGP") is
// what Discord shows the game as, and its art assets are the images.
inline constexpr const char* kDiscordApplication = "1553863705341464690";

// What Discord shows for `status` in the game called `title`. `race_start`
// is when the race's countdown ended (Unix seconds; 0 before then), and
// `session_start` when the game started.
discord::Activity describe(const GameStatus& status, const std::string& title, std::int64_t session_start,
                           std::int64_t race_start);

// "1st", "2nd", "3rd", "4th"...
std::string ordinal(int n);

class RichPresence {
 public:
  // With `log`, prints what Discord shows whenever it changes.
  RichPresence(std::string title, bool log, std::string application_id = kDiscordApplication);

  // The game's status now (called about once a second).
  void update(const GameStatus& status);

  bool connected() const { return presence_.connected(); }

 private:
  std::string title_;
  bool log_;
  discord::Presence presence_;
  discord::Activity shown_;
  std::int64_t session_start_;
  std::int64_t race_start_ = 0;
};

}  // namespace app
