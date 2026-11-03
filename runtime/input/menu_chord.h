// SPDX-License-Identifier: GPL-3.0-or-later
//
// Back + Start held together on a gamepad open the menu (for pads without a
// Guide button, or where Steam or the system takes it). By default Back is
// the coin and Start is start, and a player never presses both buttons at
// exactly the same moment, so the first one's press is held back from the
// game for a moment: if the other follows, it was the chord and the game
// never sees either; if not, the game gets the press a little late (a tap
// shorter than the wait is replayed, so none is lost).
#pragma once

#include <chrono>

#include "triforce/io_ports.h"

namespace input {

class MenuChord {
 public:
  using Clock = std::chrono::steady_clock;
  // How long the first button is held back, waiting for the second.
  static constexpr auto kWait = std::chrono::milliseconds(120);
  // How many polls a replayed tap lasts (one frame each).
  static constexpr int kReplayPolls = 3;

  // `half`: some gamepad has one of Back and Start down; `both`: some gamepad
  // has both. Changes `c`, the controls about to go to the game.
  void filter(bool half, bool both, Clock::time_point now, triforce::Controls* c) {
    if (both || chord_seen_) {
      // The chord (the menu opens), and until the buttons are all released.
      chord_seen_ = both || half;
      waiting_ = false;
      replay_ = 0;
      c->coin = c->start = false;
      return;
    }
    if (half) {
      if (!waiting_) {
        waiting_ = true;
        since_ = now;
        held_coin_ = held_start_ = false;
      }
      if (now - since_ < kWait) {
        held_coin_ |= c->coin;
        held_start_ |= c->start;
        c->coin = c->start = false;
      }
      return;
    }
    if (waiting_ && now - since_ < kWait && (held_coin_ || held_start_)) replay_ = kReplayPolls;
    waiting_ = false;
    if (replay_ > 0) {
      --replay_;
      c->coin |= held_coin_;
      c->start |= held_start_;
    }
  }

 private:
  bool waiting_ = false, chord_seen_ = false;
  Clock::time_point since_{};
  bool held_coin_ = false, held_start_ = false;
  int replay_ = 0;
};

}  // namespace input
