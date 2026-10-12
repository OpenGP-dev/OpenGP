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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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

  // Connects and disconnects controllers, and sees the pause keys pressed.
  void handle_event(const SDL_Event& event);

  // Reads the devices. Call about once a frame, on the thread that owns the
  // window.
  triforce::Controls poll();

  // Whether the pause control (OpenGP's own) was pressed since the last call:
  // its keys as handle_event() sees them, its buttons and axes as poll()
  // reads them.
  bool take_pause_press();

  // Plays the cabinet's outputs (wheel forces) on the devices.
  void set_outputs(const triforce::Outputs& outputs);

  // ---- Seeing and changing the bindings (the menu) ----

  enum class DeviceKind { Keyboard, Gamepad, Joystick };
  static constexpr std::size_t kControlCount = 12;
  // A control's name in the file ("gas") and in the menu ("Gas").
  static const char* control_name(std::size_t control);
  static const char* control_label(std::size_t control);
  // Steering takes whole axes; the other controls take keys, buttons and
  // half axes (and whole axes as pedals).
  static bool control_is_axis(std::size_t control);

  // A control's inputs on one kind of device, named as in the file.
  std::vector<std::string> bindings(DeviceKind kind, std::size_t control) const;
  void remove_binding(DeviceKind kind, std::size_t control, std::size_t index);
  // Waits for the next input of that kind (a key, a button, a hat, or an
  // axis moved well away from where it was) and adds it to the control.
  // Esc cancels.
  void start_capture(DeviceKind kind, std::size_t control);
  void cancel_capture();
  bool capturing() const;
  // Gives an event to an ongoing capture. Returns true if it used the event
  // (the menu and the game should not see it).
  bool capture_event(const SDL_Event& event);

  float force_feedback() const;  // 0 (off) to 2
  void set_force_feedback(float strength);
  float steer_speed() const;     // full turns per second for keys and buttons
  void set_steer_speed(float speed);
  float deadzone(DeviceKind kind) const;
  void set_deadzone(DeviceKind kind, float deadzone);
  void restore_defaults();

  // Writes the bindings back to the file load() read, if they changed.
  bool save(std::string* error);
  const std::string& path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace input
