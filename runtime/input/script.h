// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scripted input: the cabinet controls to hold during given frames, for
// driving the game without a player (tests, screenshots, benchmarks).
//
// A script is a list of events separated by ';' or newlines. Each event is a
// time followed by what to hold then:
//
//   600 coin                 coin at frame 600 (held for 10 frames)
//   +60 start                start, 60 frames after the previous event
//   1000-4000 gas=255 steer=100
//   +0-3000 item             a range relative to the previous event
//   # comment
//
// Frames are displayed frames (copies to the XFB). Buttons: coin, start,
// item, cancel, test, service. Analog values: steer (0-255, 128 = centre),
// gas, brake (0-255). Events that overlap are combined; later ones win for
// analog values.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "triforce/io_ports.h"

namespace input {

using namespace gekko;

class Script {
 public:
  static constexpr u64 kDefaultDuration = 10;

  // Parses `text` and appends its events. Returns false (with `error`) on a
  // syntax error, leaving the script unchanged.
  bool parse(std::string_view text, std::string* error);

  bool empty() const { return events_.empty(); }
  // The frame after the last event ends.
  u64 end() const;

  // Applies the events active at `frame` on top of `controls`.
  void apply(u64 frame, triforce::Controls& controls) const;

 private:
  struct Event {
    u64 begin = 0, end = 0;  // [begin, end)
    triforce::Controls buttons{};  // only the buttons are used
    std::optional<u8> steering, gas, brake;
  };
  std::vector<Event> events_;
};

}  // namespace input
