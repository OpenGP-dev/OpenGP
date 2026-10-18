// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player's controls (runtime/input/host.h): reading user/controls.toml,
// changing bindings as the menu does, and writing the file back.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "input/host.h"

namespace fs = std::filesystem;
using input::HostInput;
using Kind = HostInput::DeviceKind;

namespace {

fs::path temp_file(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "opengp_tests" / "controls";
  fs::create_directories(dir);
  fs::remove(dir / name);
  return dir / name;
}

std::size_t control(const char* name) {
  for (std::size_t c = 0; c < HostInput::kControlCount; ++c)
    if (std::string(HostInput::control_name(c)) == name) return c;
  FAIL("no control " << name);
  return 0;
}

using Names = std::vector<std::string>;

}  // namespace

TEST_CASE("the defaults are written the first time and read back") {
  const fs::path path = temp_file("defaults.toml");
  HostInput in;
  std::string error;
  REQUIRE_MESSAGE(in.load(path.string(), &error), error);
  CHECK(fs::exists(path));
  CHECK(in.bindings(Kind::Keyboard, control("gas")) == Names{"Up", "W"});
  CHECK(in.bindings(Kind::Gamepad, control("steer")) == Names{"leftx"});
  CHECK(in.bindings(Kind::Joystick, control("steer")) == Names{"axis0"});
}

TEST_CASE("changed bindings survive saving") {
  const fs::path path = temp_file("changed.toml");
  std::ofstream(path) << "[keyboard]\ngas = [\"Left Shift\"]\n[joystick]\ngas = [\"-axis2\"]\nbrake = [\"axis1+\", \"hat0up\"]\n";
  HostInput in;
  std::string error;
  REQUIRE_MESSAGE(in.load(path.string(), &error), error);
  CHECK(in.bindings(Kind::Keyboard, control("gas")) == Names{"Left Shift"});
  CHECK(in.bindings(Kind::Keyboard, control("brake")) == Names{"Down", "S"});  // not in the file: the default
  CHECK(in.bindings(Kind::Joystick, control("brake")) == Names{"axis1+", "hat0up"});

  // Remove one, add a key and a joystick axis the way the menu does.
  in.remove_binding(Kind::Joystick, control("brake"), 0);
  in.start_capture(Kind::Keyboard, control("item"));
  CHECK(in.capturing());
  SDL_Event e{};
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.scancode = SDL_SCANCODE_Q;
  CHECK(in.capture_event(e));
  CHECK_FALSE(in.capturing());
  in.start_capture(Kind::Joystick, control("gas"));
  e = {};
  e.type = SDL_EVENT_JOYSTICK_AXIS_MOTION;
  e.jaxis.axis = 3;
  e.jaxis.value = 5000;  // too small a move
  CHECK(in.capture_event(e));
  CHECK(in.capturing());
  e.jaxis.value = 30000;  // an axis resting in the middle, moved up: half of it
  CHECK(in.capture_event(e));
  CHECK_FALSE(in.capturing());
  // Esc cancels.
  in.start_capture(Kind::Keyboard, control("coin"));
  e = {};
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.scancode = SDL_SCANCODE_ESCAPE;
  CHECK(in.capture_event(e));
  CHECK_FALSE(in.capturing());
  in.set_force_feedback(0.5f);
  CHECK_FALSE(in.background_input());
  in.set_background_input(true);

  REQUIRE_MESSAGE(in.save(&error), error);
  HostInput again;
  REQUIRE_MESSAGE(again.load(path.string(), &error), error);
  CHECK(again.bindings(Kind::Keyboard, control("gas")) == Names{"Left Shift"});
  CHECK(again.bindings(Kind::Keyboard, control("item")) == Names{"Space", "J", "Q"});
  CHECK(again.bindings(Kind::Keyboard, control("coin")) == Names{"5", "C"});
  CHECK(again.bindings(Kind::Joystick, control("gas")) == Names{"-axis2", "axis3+"});
  CHECK(again.bindings(Kind::Joystick, control("brake")) == Names{"hat0up"});
  CHECK(again.force_feedback() == doctest::Approx(0.5f));
  CHECK(again.background_input());

  // The comments at the top are kept.
  std::ifstream f(path);
  std::string first;
  std::getline(f, first);
  CHECK(first.rfind("# OpenGP controls", 0) == 0);
}

TEST_CASE("a bad entry costs only itself, and a broken file is kept aside") {
  const fs::path path = temp_file("bad.toml");
  // An input name it does not know, and nan, which TOML allows.
  std::ofstream(path) << "[keyboard]\ngas = [\"Left Shift\", \"NoSuchKey\"]\n[gamepad]\ndeadzone = nan\n"
                         "[force_feedback]\nstrength = nan\n";
  HostInput in;
  std::string error;
  CHECK(in.load(path.string(), &error));
  CHECK(error.find("NoSuchKey") != std::string::npos);
  CHECK(in.bindings(Kind::Keyboard, control("gas")) == Names{"Left Shift"});
  CHECK(in.deadzone(Kind::Gamepad) == doctest::Approx(0.12f));  // the default's, not nan
  CHECK(in.force_feedback() == doctest::Approx(1.0f));
  in.set_deadzone(Kind::Gamepad, std::numeric_limits<float>::quiet_NaN());
  CHECK(in.deadzone(Kind::Gamepad) == in.deadzone(Kind::Gamepad));  // not NaN

  // A key SDL has no name for is not bound: capture keeps waiting.
  in.start_capture(Kind::Keyboard, control("item"));
  SDL_Event e{};
  e.type = SDL_EVENT_KEY_DOWN;
  e.key.scancode = SDL_SCANCODE_UNKNOWN;
  CHECK(in.capture_event(e));
  CHECK(in.capturing());

  // A file that cannot be parsed is kept as .bad, and the defaults are used.
  const fs::path broken = temp_file("broken.toml");
  fs::remove(fs::path(broken.string() + ".bad"));
  std::ofstream(broken) << "[keyboard\ngas = ";
  HostInput again;
  CHECK_FALSE(again.load(broken.string(), &error));
  CHECK(fs::exists(fs::path(broken.string() + ".bad")));
  CHECK(again.bindings(Kind::Keyboard, control("gas")) == Names{"Up", "W"});
}
