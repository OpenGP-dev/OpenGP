// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's input devices: the keyboard, game controllers and wheels
// (through SDL3), mapped onto the cabinet's controls, with the cabinet's
// force feedback played back on them.
//
// Bindings come from a TOML file (user/controls.toml), which is written with
// the defaults (see kDefaultConfig in host.cpp) the first time. Every
// connected device works at once.
//
// Force feedback: wheels with haptics get the game's centering spring,
// friction and roll (a push to one side); gamepads rumble when it is strong.
#pragma once

#include <memory>
#include <string>

#include "triforce/io_ports.h"

union SDL_Event;

namespace input {

using namespace gekko;

class HostInput {
 public:
  HostInput();
  ~HostInput();
  HostInput(const HostInput&) = delete;
  HostInput& operator=(const HostInput&) = delete;

  // Reads the bindings from `path`, writing the defaults there first if the
  // file does not exist. On error the defaults are used.
  bool load(const std::string& path, std::string* error);

  // Starts SDL's gamepad, joystick and haptic support.
  bool init(std::string* error);

  // Connects and disconnects controllers.
  void handle_event(const SDL_Event& event);

  // Reads the devices. Call about once a frame, on the thread that owns the
  // window.
  triforce::Controls poll();

  // Plays the cabinet's outputs (wheel forces) on the devices.
  void set_outputs(const triforce::Outputs& outputs);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace input
