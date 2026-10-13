// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/rich_presence.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace app {

namespace {

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The race: "Yoshi Cup · Round 1: Yoshi Park 1", "Time Trial · Diamond City".
std::string race_line(const GameStatus& s) {
  std::string line;
  if (!s.cup.empty() && s.round) line = s.cup + " · Round " + std::to_string(s.round);
  else if (!s.mode.empty()) line = s.mode;
  if (!s.course.empty()) line += (line.empty() ? "" : (s.round ? ": " : " · ")) + s.course;
  return line;
}

// The player in the race: "Mario · 3rd · Lap 2/3" (only "Mario" until it starts).
std::string racer_line(const GameStatus& s) {
  std::string line = s.character;
  auto add = [&line](const std::string& part) { line += (line.empty() ? "" : " · ") + part; };
  if (!s.started) return line;
  if (s.position) add(ordinal(s.position));
  if (s.lap && s.laps) add("Lap " + std::to_string(std::min(s.lap, s.laps)) + "/" + std::to_string(s.laps));
  return line;
}

}  // namespace

std::string ordinal(int n) {
  const int tens = n % 100, ones = n % 10;
  const char* suffix = (tens >= 11 && tens <= 13) ? "th" : ones == 1 ? "st" : ones == 2 ? "nd" : ones == 3 ? "rd" : "th";
  return std::to_string(n) + suffix;
}

discord::Activity describe(const GameStatus& s, const std::string& title, std::int64_t session_start,
                           std::int64_t race_start) {
  discord::Activity a;
  a.details = title;
  a.start = session_start;
  a.large_image = s.game_image;
  a.large_text = title;
  switch (s.scene) {
    case GameStatus::Scene::Starting: a.state = "Starting up"; break;
    case GameStatus::Scene::Attract: a.state = "In the attract mode"; break;
    case GameStatus::Scene::Menus: a.state = s.activity; break;
    case GameStatus::Scene::TestMenu: a.state = "In the test menu"; break;
    case GameStatus::Scene::Racing:
    case GameStatus::Scene::Finished: {
      if (std::string line = race_line(s); !line.empty()) a.details = line;
      a.state = s.scene == GameStatus::Scene::Racing ? racer_line(s) : "Race over";
      if (s.scene == GameStatus::Scene::Racing && race_start) a.start = race_start;
      if (!s.cup_image.empty()) {
        a.large_image = s.cup_image;
        a.large_text = s.cup + (s.engine.empty() ? "" : ", " + s.engine);
      }
      break;
    }
  }
  if (s.scene != GameStatus::Scene::Starting && s.scene != GameStatus::Scene::Attract &&
      !s.character_image.empty()) {
    a.small_image = s.character_image;
    a.small_text = s.character;
  }
  return a;
}

RichPresence::RichPresence(std::string title, bool log, std::string application_id)
    : title_(std::move(title)), log_(log), presence_(std::move(application_id)), session_start_(unix_now()) {}

void RichPresence::update(const GameStatus& status) {
  const bool counting = status.scene == GameStatus::Scene::Racing && status.started;
  if (!counting) race_start_ = 0;
  else if (!race_start_) race_start_ = unix_now();
  discord::Activity activity = describe(status, title_, session_start_, race_start_);
  if (log_ && activity != shown_) {
    std::fprintf(stderr, "[discord] %s | %s | %s, %s\n", activity.details.c_str(), activity.state.c_str(),
                 activity.large_image.c_str(), activity.small_image.c_str());
  }
  shown_ = activity;
  presence_.set(std::move(activity));
}

}  // namespace app
