// SPDX-License-Identifier: GPL-3.0-or-later

#include "input/script.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace input {

namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

// Parses a whole token as a number (decimal, or hex with 0x).
bool parse_number(std::string_view s, u64* out) {
  int radix = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s.remove_prefix(2);
    radix = 16;
  }
  if (s.empty()) return false;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), *out, radix);
  return ec == std::errc{} && end == s.data() + s.size();
}

}  // namespace

bool Script::parse(std::string_view text, std::string* error) {
  std::vector<Event> parsed;
  u64 previous = events_.empty() ? 0 : events_.back().begin;
  int line = 0;
  auto fail = [&](std::string_view event, const std::string& why) {
    if (error) *error = "input script, event " + std::to_string(line) + " (\"" + std::string(event) + "\"): " + why;
    return false;
  };

  while (!text.empty()) {
    const std::size_t split = text.find_first_of(";\n");
    std::string_view event = text.substr(0, split);
    text = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    ++line;
    if (const std::size_t hash = event.find('#'); hash != std::string_view::npos) event = event.substr(0, hash);
    event = trim(event);
    if (event.empty()) continue;

    // The time: FRAME, FRAME-END, +DELAY or +DELAY-LENGTH.
    const std::size_t space = event.find_first_of(" \t");
    std::string_view when = event.substr(0, space);
    std::string_view what = space == std::string_view::npos ? std::string_view{} : trim(event.substr(space));
    const bool relative = !when.empty() && when[0] == '+';
    if (relative) when.remove_prefix(1);
    const std::size_t dash = when.find('-');
    u64 begin = 0, end = 0;
    if (!parse_number(when.substr(0, dash), &begin)) return fail(event, "expected a frame number");
    if (relative) begin += previous;
    if (dash == std::string_view::npos) {
      end = begin + kDefaultDuration;
    } else {
      u64 second = 0;
      if (!parse_number(when.substr(dash + 1), &second)) return fail(event, "expected a frame number after '-'");
      end = relative ? begin + second : second;
      if (end <= begin) return fail(event, "the range is empty");
    }
    previous = begin;

    Event e;
    e.begin = begin;
    e.end = end;
    if (what.empty()) return fail(event, "nothing to press");
    while (!what.empty()) {
      const std::size_t next = what.find_first_of(" \t,");
      const std::string_view item = what.substr(0, next);
      what = next == std::string_view::npos ? std::string_view{} : trim(what.substr(next + 1));
      if (item.empty()) continue;
      const std::size_t eq = item.find('=');
      if (eq == std::string_view::npos) {
        if (item == "coin") e.buttons.coin = true;
        else if (item == "start") e.buttons.start = true;
        else if (item == "item") e.buttons.item = true;
        else if (item == "cancel") e.buttons.cancel = true;
        else if (item == "test") e.buttons.test = true;
        else if (item == "service") e.buttons.service = true;
        else return fail(event, "unknown button \"" + std::string(item) + "\"");
        continue;
      }
      const std::string_view name = item.substr(0, eq);
      u64 value = 0;
      if (!parse_number(item.substr(eq + 1), &value) || value > 255) {
        return fail(event, "\"" + std::string(name) + "\" needs a value from 0 to 255");
      }
      const u8 v = static_cast<u8>(value);
      if (name == "steer") e.steering = v;
      else if (name == "gas") e.gas = v;
      else if (name == "brake") e.brake = v;
      else return fail(event, "unknown control \"" + std::string(name) + "\"");
    }
    parsed.push_back(e);
  }
  events_.insert(events_.end(), parsed.begin(), parsed.end());
  return true;
}

u64 Script::end() const {
  u64 last = 0;
  for (const Event& e : events_) last = std::max(last, e.end);
  return last;
}

void Script::apply(u64 frame, triforce::Controls& c) const {
  for (const Event& e : events_) {
    if (frame < e.begin || frame >= e.end) continue;
    const triforce::Controls& b = e.buttons;
    c.coin |= b.coin;
    c.start |= b.start;
    c.item |= b.item;
    c.cancel |= b.cancel;
    c.test |= b.test;
    c.service |= b.service;
    // 0-255 as on the cabinet's 8-bit scale, 128 straight ahead.
    if (e.steering) c.steering = std::clamp((static_cast<float>(*e.steering) - 128.0f) / 127.0f, -1.0f, 1.0f);
    if (e.gas) c.gas = static_cast<float>(*e.gas) / 255.0f;
    if (e.brake) c.brake = static_cast<float>(*e.brake) / 255.0f;
  }
}

}  // namespace input
