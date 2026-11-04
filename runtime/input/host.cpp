// SPDX-License-Identifier: GPL-3.0-or-later

#include "input/host.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "input/menu_chord.h"
#include "os/atomic_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace input {

namespace {

constexpr const char* kDefaultConfig = R"(# OpenGP controls. Delete this file to get the defaults back.
#
# Each control lists the inputs that drive it; every connected device works
# at once.
#
#   [keyboard]  SDL key names: "Left", "A", "Space", "Return", "Left Shift", "F1"
#   [gamepad]   controllers SDL knows (Xbox, PlayStation, Switch and so on).
#               Buttons: a b x y back guide start leftstick rightstick
#               leftshoulder rightshoulder dpup dpdown dpleft dpright misc1
#               (a b x y are positions: bottom, right, left, top).
#               Axes: leftx lefty rightx righty lefttrigger righttrigger
#   [joystick]  other devices, such as most wheels and pedals: "button0",
#               "axis0", "hat0up" (hat0down, hat0left, hat0right)
#
# Axes: "axis2" is the whole axis, "axis2+" and "axis2-" one half of it, and
# a leading "-" turns it around. As a pedal, a whole axis goes from released
# at its minimum to pressed at its maximum ("-axis2" for a pedal that reads
# highest when released).
#
# Steering: "steer" takes whole axes (a wheel, a stick); "steer_left" and
# "steer_right" take keys, buttons or half axes.
#
# "pause" is OpenGP's, not the cabinet's: it freezes the game until pressed
# again.
#
# The last four are the profiler's: "profile_toggle" starts and stops a
# recording, "profile_mark" marks a moment, "profile_capture" captures the
# next frame, and "profile_overlay" shows the live performance overlay. They
# can go on any key, button or axis, like the cabinet's controls.

[keyboard]
steer_left = ["Left", "A"]
steer_right = ["Right", "D"]
gas = ["Up", "W"]
brake = ["Down", "S"]
item = ["Space", "J"]
cancel = ["Backspace", "K"]
start = ["Return", "1"]
coin = ["5", "C"]
service = ["F1"]
test = ["F2"]
pause = ["P", "Pause"]
profile_toggle = ["F10"]
profile_mark = ["F8"]
profile_capture = ["F9"]
profile_overlay = ["F7"]

[gamepad]
steer = ["leftx"]
steer_left = ["dpleft"]
steer_right = ["dpright"]
gas = ["righttrigger"]
brake = ["lefttrigger"]
item = ["a", "rightshoulder"]
cancel = ["b", "leftshoulder"]
start = ["start"]
coin = ["back"]
# The profiler's actions are unbound here: add buttons ("y", "rightstick"...)
# to record, mark, capture or show the overlay from a controller.
profile_toggle = []
profile_mark = []
profile_capture = []
profile_overlay = []
deadzone = 0.12

[joystick]
# Wheels differ: set these up for yours. Pedals are often "-axis1", "-axis2"
# and so on.
steer = ["axis0"]
gas = []
brake = []
item = ["button0"]
cancel = ["button1"]
start = ["button9"]
coin = []
profile_toggle = []
profile_mark = []
profile_capture = []
profile_overlay = []
deadzone = 0.0

[steering]
# Keys and buttons turn the wheel gradually: full turns per second.
speed = 4.0

[force_feedback]
# 0 turns it off, 1 is the game's full strength.
strength = 1.0
# Gamepads rumble on bumps and hits.
rumble = true
)";

// Where a value comes from.
struct Source {
  enum class Kind : u8 { Key, PadButton, PadAxis, JoyButton, JoyAxis, JoyHat };
  enum class Range : u8 { Whole, Positive, Negative };
  Kind kind = Kind::Key;
  int index = 0;  // scancode, button, axis or hat
  u8 hat = 0;     // SDL_HAT_* mask
  Range range = Range::Whole;
  bool invert = false;
};

enum Control : std::size_t {
  kSteer,  // whole axes
  kSteerLeft,
  kSteerRight,
  kGas,
  kBrake,
  kItem,
  kCancel,
  kStart,
  kCoin,
  kTest,
  kService,
  kPause,  // OpenGP's, not the cabinet's
  kProfileToggle,
  kProfileMark,
  kProfileCapture,
  kProfileOverlay,
  kControlCount
};
constexpr std::array<const char*, kControlCount> kControlNames = {
    "steer",          "steer_left",   "steer_right",    "gas",         "brake",
    "item",           "cancel",       "start",          "coin",        "test",
    "service",        "pause",        "profile_toggle", "profile_mark", "profile_capture",
    "profile_overlay"};
constexpr std::array<const char*, kControlCount> kControlLabels = {
    "Steering",       "Steer left",   "Steer right",    "Gas",          "Brake",
    "Item button",    "Cancel button", "Start",          "Coin",         "Test",
    "Service",        "Pause",        "Start/stop profile", "Mark moment", "Capture frame",
    "Profiler overlay"};
static_assert(kControlCount == HostInput::kControlCount);
static_assert(kProfileToggle == HostInput::kProfilerFirst);

// The bindings of one kind of device.
struct Bindings {
  std::array<std::vector<Source>, kControlCount> sources;
  float deadzone = 0.0f;
};

struct Config {
  Bindings keyboard, gamepad, joystick;
  float steer_speed = 4.0f;
  float ffb_strength = 1.0f;
  bool rumble = true;
  bool background = false;  // read the controls while the window is in the background
};

// Reading keys in the background (set from the controls' setting).
bool g_background_keys = false;

// Whether a key is down, asked of the system rather than of the window, so
// that it works while the window is in the background. Windows only: keys
// are matched by position (SDL's scancodes are USB usages), through the PC
// scancode and the keyboard layout's virtual key; keys that do not move with
// the layout go straight to their virtual key.
#ifdef _WIN32
bool system_key_down(int scancode) {
  static constexpr struct {
    int usage;
    UINT code;  // the PC scancode
  } kKeys[] = {
      {4, 0x1E}, {5, 0x30}, {6, 0x2E}, {7, 0x20}, {8, 0x12}, {9, 0x21}, {10, 0x22}, {11, 0x23}, {12, 0x17},
      {13, 0x24}, {14, 0x25}, {15, 0x26}, {16, 0x32}, {17, 0x31}, {18, 0x18}, {19, 0x19}, {20, 0x10}, {21, 0x13},
      {22, 0x1F}, {23, 0x14}, {24, 0x16}, {25, 0x2F}, {26, 0x11}, {27, 0x2D}, {28, 0x15}, {29, 0x2C},  // A-Z
      {30, 0x02}, {31, 0x03}, {32, 0x04}, {33, 0x05}, {34, 0x06}, {35, 0x07}, {36, 0x08}, {37, 0x09}, {38, 0x0A},
      {39, 0x0B},  // 1-0
      {45, 0x0C}, {46, 0x0D}, {47, 0x1A}, {48, 0x1B}, {49, 0x2B}, {51, 0x27}, {52, 0x28}, {53, 0x29}, {54, 0x33},
      {55, 0x34}, {56, 0x35}, {100, 0x56},  // punctuation, and the key left of Z on ISO keyboards
  };
  static constexpr struct {
    int usage;
    UINT vk;
  } kVirtualKeys[] = {
      {40, VK_RETURN}, {41, VK_ESCAPE}, {42, VK_BACK}, {43, VK_TAB}, {44, VK_SPACE}, {57, VK_CAPITAL},
      {58, VK_F1}, {59, VK_F2}, {60, VK_F3}, {61, VK_F4}, {62, VK_F5}, {63, VK_F6}, {64, VK_F7}, {65, VK_F8},
      {66, VK_F9}, {67, VK_F10}, {68, VK_F11}, {69, VK_F12}, {72, VK_PAUSE}, {73, VK_INSERT}, {74, VK_HOME},
      {75, VK_PRIOR}, {76, VK_DELETE}, {77, VK_END}, {78, VK_NEXT}, {79, VK_RIGHT}, {80, VK_LEFT}, {81, VK_DOWN},
      {82, VK_UP}, {84, VK_DIVIDE}, {85, VK_MULTIPLY}, {86, VK_SUBTRACT}, {87, VK_ADD}, {88, VK_RETURN},
      {89, VK_NUMPAD1}, {90, VK_NUMPAD2}, {91, VK_NUMPAD3}, {92, VK_NUMPAD4}, {93, VK_NUMPAD5}, {94, VK_NUMPAD6},
      {95, VK_NUMPAD7}, {96, VK_NUMPAD8}, {97, VK_NUMPAD9}, {98, VK_NUMPAD0}, {99, VK_DECIMAL},
      {224, VK_LCONTROL}, {225, VK_LSHIFT}, {226, VK_LMENU}, {227, VK_LWIN}, {228, VK_RCONTROL}, {229, VK_RSHIFT},
      {230, VK_RMENU}, {231, VK_RWIN},
  };
  UINT vk = 0;
  for (const auto& k : kKeys)
    if (k.usage == scancode) vk = MapVirtualKeyW(k.code, MAPVK_VSC_TO_VK_EX);
  for (const auto& k : kVirtualKeys)
    if (k.usage == scancode) vk = k.vk;
  return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000);
}
#endif

bool is_trigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

// Parses one input name for a kind of device. Returns false if unknown.
bool parse_source(std::string_view name, Source::Kind device, Source* out) {
  Source s;
  if (device == Source::Kind::Key) {
    const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
    if (code == SDL_SCANCODE_UNKNOWN) return false;
    s.kind = Source::Kind::Key;
    s.index = code;
    *out = s;
    return true;
  }
  if (!name.empty() && name.front() == '-') {
    s.invert = true;
    name.remove_prefix(1);
  }
  if (!name.empty() && (name.back() == '+' || name.back() == '-')) {
    s.range = name.back() == '+' ? Source::Range::Positive : Source::Range::Negative;
    name.remove_suffix(1);
  }
  auto number_after = [&](std::string_view prefix, int* value) {
    if (name.substr(0, prefix.size()) != prefix) return false;
    const std::string_view digits = name.substr(prefix.size());
    if (digits.empty() || digits.size() > 3) return false;
    int v = 0;
    for (const char c : digits) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    *value = v;
    return true;
  };
  if (device == Source::Kind::PadButton) {
    const std::string n(name);
    if (const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(n.c_str()); b != SDL_GAMEPAD_BUTTON_INVALID) {
      s.kind = Source::Kind::PadButton;
      s.index = b;
    } else if (const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(n.c_str()); a != SDL_GAMEPAD_AXIS_INVALID) {
      s.kind = Source::Kind::PadAxis;
      s.index = a;
    } else {
      return false;
    }
  } else if (number_after("button", &s.index)) {
    s.kind = Source::Kind::JoyButton;
  } else if (number_after("axis", &s.index)) {
    s.kind = Source::Kind::JoyAxis;
  } else if (name.substr(0, 3) == "hat") {
    // hat<N><direction>
    static constexpr std::pair<std::string_view, u8> kHats[] = {
        {"up", SDL_HAT_UP}, {"down", SDL_HAT_DOWN}, {"left", SDL_HAT_LEFT}, {"right", SDL_HAT_RIGHT}};
    const std::size_t digits_end = name.find_first_not_of("0123456789", 3);
    if (digits_end == 3 || digits_end == std::string_view::npos || digits_end > 6) return false;
    const auto it = std::find_if(std::begin(kHats), std::end(kHats),
                                 [&](const auto& h) { return h.first == name.substr(digits_end); });
    if (it == std::end(kHats)) return false;
    s.kind = Source::Kind::JoyHat;
    s.index = std::stoi(std::string(name.substr(3, digits_end - 3)));
    s.hat = it->second;
  } else {
    return false;
  }
  *out = s;
  return true;
}

bool parse_bindings(const toml::table* table, Source::Kind device, const char* section, Bindings* out,
                    std::string* error) {
  if (!table) return true;  // keep the defaults
  for (std::size_t c = 0; c < kControlCount; ++c) {
    const toml::node* node = table->get(kControlNames[c]);
    if (!node) continue;
    std::vector<Source> sources;
    // An input it does not know is left out, with a warning: the rest of the
    // file still counts.
    auto add = [&](const toml::node& item) {
      const auto name = item.value<std::string>();
      Source s;
      if (!name || !parse_source(*name, device, &s)) {
        *error += std::string(error->empty() ? "" : "; ") + "[" + section + "] " + kControlNames[c] +
                  ": unknown input \"" + (name ? *name : std::string("?")) + "\" (left out)";
        return;
      }
      sources.push_back(s);
    };
    if (const toml::array* list = node->as_array()) {
      for (const toml::node& item : *list) add(item);
    } else {
      add(*node);
    }
    out->sources[c] = std::move(sources);
  }
  if (const auto deadzone = (*table)["deadzone"].value<double>(); deadzone && std::isfinite(*deadzone)) {
    out->deadzone = std::clamp(static_cast<float>(*deadzone), 0.0f, 0.9f);
  }
  return true;
}

// Parses a config over `config`: what the text leaves out stays as it was.
bool parse_config(std::string_view text, Config* config, std::string* error) {
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    const toml::source_position pos = parsed.error().source().begin;
    *error = "line " + std::to_string(pos.line) + ": " + std::string(parsed.error().description());
    return false;
  }
  const toml::table& root = parsed.table();
  Config c = *config;
  if (!parse_bindings(root["keyboard"].as_table(), Source::Kind::Key, "keyboard", &c.keyboard, error) ||
      !parse_bindings(root["gamepad"].as_table(), Source::Kind::PadButton, "gamepad", &c.gamepad, error) ||
      !parse_bindings(root["joystick"].as_table(), Source::Kind::JoyButton, "joystick", &c.joystick, error)) {
    return false;
  }
  // nan and inf are TOML numbers too: they would pass the clamps.
  if (const double v = root["steering"]["speed"].value_or(double{c.steer_speed}); std::isfinite(v))
    c.steer_speed = std::max(0.1f, static_cast<float>(v));
  if (const double v = root["force_feedback"]["strength"].value_or(double{c.ffb_strength}); std::isfinite(v))
    c.ffb_strength = std::clamp(static_cast<float>(v), 0.0f, 2.0f);
  c.rumble = root["force_feedback"]["rumble"].value_or(c.rumble);
  c.background = root["background"]["input"].value_or(c.background);
  *config = std::move(c);
  return true;
}

// An input's name in the file (the inverse of parse_source).
std::string source_name(const Source& s) {
  std::string name;
  switch (s.kind) {
    case Source::Kind::Key: return SDL_GetScancodeName(static_cast<SDL_Scancode>(s.index));
    case Source::Kind::PadButton: {
      const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(s.index));
      return n ? n : "";
    }
    case Source::Kind::JoyButton: return "button" + std::to_string(s.index);
    case Source::Kind::JoyHat: {
      const char* dir = s.hat == SDL_HAT_UP ? "up" : s.hat == SDL_HAT_DOWN ? "down" : s.hat == SDL_HAT_LEFT ? "left" : "right";
      return "hat" + std::to_string(s.index) + dir;
    }
    case Source::Kind::PadAxis: {
      const char* n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(s.index));
      name = n ? n : "";
      break;
    }
    case Source::Kind::JoyAxis: name = "axis" + std::to_string(s.index); break;
  }
  if (s.range == Source::Range::Positive) name += '+';
  else if (s.range == Source::Range::Negative) name += '-';
  if (s.invert) name = "-" + name;
  return name;
}

bool same_source(const Source& a, const Source& b) {
  return a.kind == b.kind && a.index == b.index && a.hat == b.hat && a.range == b.range && a.invert == b.invert;
}

// The file for `config`: the default file's comments, then the bindings.
std::string config_text(const Config& c) {
  const std::string_view defaults(kDefaultConfig);
  std::string out(defaults.substr(0, defaults.find("\n[keyboard]") + 1));  // the comments before the first section
  char number[32];
  auto real = [&](float v) {
    std::snprintf(number, sizeof(number), "%.2f", static_cast<double>(v));
    return std::string(number);
  };
  auto section = [&](const char* name, const Bindings& b, bool axes, bool deadzone) {
    out += std::string("[") + name + "]\n";
    if (std::string_view(name) == "joystick") {
      out += "# Wheels differ: set these up for yours. Pedals are often \"-axis1\", \"-axis2\"\n# and so on.\n";
    }
    for (std::size_t control = 0; control < kControlCount; ++control) {
      if (control == kSteer && !axes) continue;
      out += std::string(kControlNames[control]) + " = [";
      bool first = true;
      for (std::size_t i = 0; i < b.sources[control].size(); ++i) {
        std::string name = source_name(b.sources[control][i]);
        if (name.empty()) continue;  // an input with no name could not be read back
        std::string quoted = "\"";
        for (const char ch : name) {
          if (ch == '"' || ch == '\\') quoted += '\\';
          quoted += ch;
        }
        out += (first ? "" : ", ") + quoted + "\"";
        first = false;
      }
      out += "]\n";
    }
    if (deadzone) out += "deadzone = " + real(b.deadzone) + "\n";
    out += "\n";
  };
  section("keyboard", c.keyboard, false, false);
  section("gamepad", c.gamepad, true, true);
  section("joystick", c.joystick, true, true);
  out += "[steering]\n# Keys and buttons turn the wheel gradually: full turns per second.\nspeed = " + real(c.steer_speed) +
         "\n\n[force_feedback]\n# 0 turns it off, 1 is the game's full strength.\nstrength = " + real(c.ffb_strength) +
         "\n# Gamepads rumble on bumps and hits.\nrumble = " + (c.rumble ? "true" : "false") +
         "\n\n[background]\n# Read the controls while the window is in the background too (to drive several\n"
         "# copies of OpenGP at once): gamepads, and the keyboard on Windows.\ninput = " +
         (c.background ? "true" : "false") + "\n";
  return out;
}

float apply_deadzone(float v, float deadzone) {
  const float m = std::fabs(v);
  if (m <= deadzone) return 0.0f;
  return std::copysign((m - deadzone) / (1.0f - deadzone), v);
}

}  // namespace

// A capture's baseline for an axis that hasn't reported yet (outside -32768..32767).
constexpr int kUnknownAxis = 1 << 20;

struct HostInput::Impl {
  struct Device {
    SDL_Gamepad* gamepad = nullptr;
    SDL_Joystick* joystick = nullptr;  // owned only without a gamepad
    SDL_Haptic* haptic = nullptr;
    // Wheel effects (-1: not supported).
    int spring = -1, friction = -1, torque = -1;
    bool wheel = false;  // SDL says it is a steering wheel
    float sent[3] = {-2.0f, -2.0f, -2.0f};  // the spring, friction and roll last sent
    int axes = 0, buttons = 0, hats = 0;
  };

  Config config;
  std::string path;
  MenuChord menu_chord;
  bool dirty = false;      // bindings changed since load() or save()
  bool keep_file = false;  // the file could not be read nor set aside: never overwritten

  // Gamepads' events come to a window in the background only if SDL is told.
  void apply_background() {
    g_background_keys = config.background && background_keyboard_supported();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, config.background ? "1" : "0");
  }
  static bool background_keyboard_supported() { return HostInput::background_keyboard(); }
  std::map<SDL_JoystickID, Device> devices;
  // Binding an input from the menu: waiting for one of `kind`, for `control`.
  struct Capture {
    bool active = false;
    DeviceKind kind = DeviceKind::Keyboard;
    std::size_t control = 0;
    std::map<SDL_JoystickID, std::vector<int>> axes;  // where each axis was at the start, or kUnknownAxis
  } capture;
  float digital_steer = 0.0f;  // -1..1, keys and buttons turn it gradually
  bool pause_held = false;     // the pause control's buttons and axes, at the last poll
  bool pause_pressed = false;  // pressed since take_pause_press()
  u32 profiler_held = 0;       // the profiler's controls' inputs, at the last poll
  u32 profiler_pressed = 0;    // ... pressed since take_profiler_actions()
  std::chrono::steady_clock::time_point last_poll = std::chrono::steady_clock::now();
  triforce::Outputs outputs;
  std::chrono::steady_clock::time_point last_play{};

  void open(SDL_JoystickID id);
  void close(SDL_JoystickID id);
  Bindings& bindings(DeviceKind kind) {
    return kind == DeviceKind::Keyboard ? config.keyboard : kind == DeviceKind::Gamepad ? config.gamepad : config.joystick;
  }
  bool finish_capture(const Source& source);
  void start_effects(Device& d);
  void play(Device& d);
  static SDL_HapticEffect condition_effect(Uint16 type, float amount);
  static SDL_HapticEffect constant_effect(float level);

  // A source's raw value: -1..1 for whole axes, 0..1 otherwise.
  static float raw(const Source& s, const Device* d) {
    switch (s.kind) {
      case Source::Kind::Key: {
#ifdef _WIN32
        if (g_background_keys) return system_key_down(s.index) ? 1.0f : 0.0f;
#endif
        int count = 0;
        const bool* keys = SDL_GetKeyboardState(&count);
        return s.index < count && keys[s.index] ? 1.0f : 0.0f;
      }
      case Source::Kind::PadButton: return SDL_GetGamepadButton(d->gamepad, static_cast<SDL_GamepadButton>(s.index)) ? 1.0f : 0.0f;
      case Source::Kind::PadAxis: {
        const float v = SDL_GetGamepadAxis(d->gamepad, static_cast<SDL_GamepadAxis>(s.index)) / 32767.0f;
        return std::clamp(v, -1.0f, 1.0f);
      }
      case Source::Kind::JoyButton: return SDL_GetJoystickButton(d->joystick, s.index) ? 1.0f : 0.0f;
      case Source::Kind::JoyAxis: return std::clamp(SDL_GetJoystickAxis(d->joystick, s.index) / 32767.0f, -1.0f, 1.0f);
      case Source::Kind::JoyHat: return (SDL_GetJoystickHat(d->joystick, s.index) & s.hat) ? 1.0f : 0.0f;
    }
    return 0.0f;
  }
  static bool is_axis(const Source& s) { return s.kind == Source::Kind::PadAxis || s.kind == Source::Kind::JoyAxis; }
  // Whether the device has the input at all: SDL reads one it lacks as 0,
  // which a whole-axis pedal would take as half pressed.
  static bool present(const Source& s, const Device* d) {
    if (!d) return true;
    switch (s.kind) {
      case Source::Kind::JoyAxis: return s.index < d->axes;
      case Source::Kind::JoyButton: return s.index < d->buttons;
      case Source::Kind::JoyHat: return s.index < d->hats;
      default: return true;
    }
  }
  // As a whole axis (steering): -1..1.
  static float axis_value(const Source& s, const Device* d, float deadzone) {
    float v = raw(s, d);
    if (s.invert) v = -v;
    return apply_deadzone(v, deadzone);
  }
  // Whether a joystick axis has reported where it is. Until a device sends
  // its first report SDL reads every axis as 0, the middle: a pedal that
  // rests at one end would read half pressed until it was first moved.
  static bool reported(const Source& s, const Device* d) {
    if (s.kind != Source::Kind::JoyAxis || !d || !d->joystick) return true;
    Sint16 initial = 0;
    return SDL_GetJoystickAxisInitialState(d->joystick, s.index, &initial);
  }
  // As a pedal or button: 0..1.
  static float amount(const Source& s, const Device* d, float deadzone) {
    if (!reported(s, d)) return 0.0f;  // at rest, wherever that is
    float v = raw(s, d);
    if (!is_axis(s)) return v;
    if (s.invert) v = -v;
    if (s.range == Source::Range::Positive) v = std::max(v, 0.0f);
    else if (s.range == Source::Range::Negative) v = std::max(-v, 0.0f);
    else if (!(s.kind == Source::Kind::PadAxis && is_trigger(s.index))) v = (v + 1.0f) * 0.5f;  // triggers are 0..1
    return std::clamp((v - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
  }
};

HostInput::HostInput() : impl_(std::make_unique<Impl>()) {}

void HostInput::stop_outputs() {
  for (auto& [id, d] : impl_->devices) {
    if (d.haptic) SDL_StopHapticEffects(d.haptic);
    for (float& s : d.sent) s = -2.0f;
    if (d.gamepad) SDL_RumbleGamepad(d.gamepad, 0, 0, 0);
  }
}

HostInput::~HostInput() {
  if (!impl_) return;
  std::vector<SDL_JoystickID> ids;
  for (const auto& [id, d] : impl_->devices) ids.push_back(id);
  for (const SDL_JoystickID id : ids) impl_->close(id);
}

bool HostInput::load(const std::string& path, std::string* error) {
  namespace fs = std::filesystem;
  impl_->path = path;
  impl_->dirty = false;
  std::error_code ec;
  const fs::path file = os::utf8_path(path);
  if (!fs::exists(file, ec)) {
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << kDefaultConfig;
  }
  std::string text = kDefaultConfig;
  if (std::ifstream f(file, std::ios::binary); f) {
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::string why;
  impl_->config = Config{};
  parse_config(kDefaultConfig, &impl_->config, &why);
  why.clear();
  const bool ok = parse_config(text, &impl_->config, &why);
  if (!ok) {
    // Kept aside, not overwritten by the next save: the player can fix it.
    fs::rename(file, os::utf8_path(path + ".bad"), ec);
    if (ec) {
      impl_->keep_file = true;  // never overwrite it
      if (error)
        *error = path + ": " + why + ". The default controls are used, and changes are not saved until it is fixed.";
    } else {
      impl_->dirty = true;
      if (error) *error = path + ": " + why + ". It was kept as controls.toml.bad; the default controls are used.";
    }
  } else if (!why.empty() && error) {
    *error = path + ": " + why;
  }
  impl_->apply_background();
  return ok;
}

bool HostInput::init(std::string* error) {
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
    if (error) *error = SDL_GetError();
    return false;
  }
  SDL_InitSubSystem(SDL_INIT_HAPTIC);  // optional
  return true;
}

void HostInput::handle_event(const SDL_Event& e) {
  if (e.type == SDL_EVENT_JOYSTICK_ADDED) impl_->open(e.jdevice.which);
  else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) impl_->close(e.jdevice.which);
  // Pause keys go by their key presses, which a poll can miss if they are
  // quick (the game's controls are held). The profiler's actions are quick
  // presses too: a key, or a button on a controller.
  auto press = [&](const Bindings& b, Source::Kind kind, int index) {
    for (std::size_t c = kProfilerFirst; c < kControlCount; ++c)
      for (const Source& s : b.sources[c])
        if (s.kind == kind && s.index == index) impl_->profiler_pressed |= 1u << (c - kProfilerFirst);
  };
  if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
    for (const Source& s : impl_->config.keyboard.sources[kPause])
      if (s.kind == Source::Kind::Key && s.index == static_cast<int>(e.key.scancode)) impl_->pause_pressed = true;
    press(impl_->config.keyboard, Source::Kind::Key, e.key.scancode);
  } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    press(impl_->config.gamepad, Source::Kind::PadButton, e.gbutton.button);
  } else if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
    press(impl_->config.joystick, Source::Kind::JoyButton, e.jbutton.button);
  }
}

void HostInput::Impl::open(SDL_JoystickID id) {
  if (devices.count(id)) return;
  Device d;
  if (SDL_IsGamepad(id)) {
    d.gamepad = SDL_OpenGamepad(id);
    if (d.gamepad) d.joystick = SDL_GetGamepadJoystick(d.gamepad);
  } else {
    d.joystick = SDL_OpenJoystick(id);
  }
  if (!d.joystick) return;
  d.wheel = SDL_GetJoystickType(d.joystick) == SDL_JOYSTICK_TYPE_WHEEL;
  d.axes = SDL_GetNumJoystickAxes(d.joystick);
  d.buttons = SDL_GetNumJoystickButtons(d.joystick);
  d.hats = SDL_GetNumJoystickHats(d.joystick);
  const char* name = d.gamepad ? SDL_GetGamepadName(d.gamepad) : SDL_GetJoystickName(d.joystick);
  std::fprintf(stderr, "[input] %s connected: %s\n", d.gamepad ? "gamepad" : "joystick", name ? name : "?");
  d.haptic = SDL_OpenHapticFromJoystick(d.joystick);
  if (d.haptic) start_effects(d);
  devices[id] = d;
  play(devices[id]);
}

void HostInput::Impl::close(SDL_JoystickID id) {
  const auto it = devices.find(id);
  if (it == devices.end()) return;
  Device& d = it->second;
  if (d.haptic) SDL_CloseHaptic(d.haptic);
  if (d.gamepad) SDL_CloseGamepad(d.gamepad);
  else if (d.joystick) SDL_CloseJoystick(d.joystick);
  devices.erase(it);
}

// Wheels: a spring and friction on the steering axis, and a constant force
// for the roll. Started once, then updated as the game changes them.
void HostInput::Impl::start_effects(Device& d) {
  const Uint32 features = SDL_GetHapticFeatures(d.haptic);
  if (features & SDL_HAPTIC_AUTOCENTER) SDL_SetHapticAutocenter(d.haptic, 0);
  if (features & SDL_HAPTIC_GAIN) SDL_SetHapticGain(d.haptic, 100);
  auto create = [&](Uint32 feature, const SDL_HapticEffect& effect) -> int {
    if (!(features & feature)) return -1;
    const int id = SDL_CreateHapticEffect(d.haptic, &effect);
    if (id >= 0) SDL_RunHapticEffect(d.haptic, id, 1);
    return id;
  };
  d.spring = create(SDL_HAPTIC_SPRING, condition_effect(SDL_HAPTIC_SPRING, 0.0f));
  d.friction = create(SDL_HAPTIC_FRICTION, condition_effect(SDL_HAPTIC_FRICTION, 0.0f));
  d.torque = create(SDL_HAPTIC_CONSTANT, constant_effect(0.0f));
  if (d.spring >= 0 || d.friction >= 0 || d.torque >= 0) {
    std::fprintf(stderr, "[input] force feedback:%s%s%s\n", d.spring >= 0 ? " spring" : "",
                 d.friction >= 0 ? " friction" : "", d.torque >= 0 ? " torque" : "");
  }
}

SDL_HapticEffect HostInput::Impl::condition_effect(Uint16 type, float amount) {
  SDL_HapticEffect e{};
  e.condition.type = type;
  e.condition.direction.type = SDL_HAPTIC_STEERING_AXIS;
  e.condition.length = SDL_HAPTIC_INFINITY;
  const auto coeff = static_cast<Sint16>(amount * 32767.0f);
  e.condition.right_sat[0] = e.condition.left_sat[0] = 0xFFFF;
  e.condition.right_coeff[0] = e.condition.left_coeff[0] = coeff;
  return e;
}

SDL_HapticEffect HostInput::Impl::constant_effect(float level) {
  SDL_HapticEffect e{};
  e.constant.type = SDL_HAPTIC_CONSTANT;
  e.constant.direction.type = SDL_HAPTIC_STEERING_AXIS;
  e.constant.length = SDL_HAPTIC_INFINITY;
  e.constant.level = static_cast<Sint16>(level * 32767.0f);
  return e;
}

// The game's forces: centering and friction are 0..0x7FFF; roll is a signed
// push on the wheel, 0x8000 +/- 0x7FFF (the road's buzz, bumps and hits), or 0
// when off.
void HostInput::Impl::play(Device& d) {
  const float strength = config.ffb_strength;
  const float centering = std::clamp(outputs.centering / 32767.0f * strength, 0.0f, 1.0f);
  const float friction = std::clamp(outputs.friction / 32767.0f * strength, 0.0f, 1.0f);
  const float roll = outputs.roll ? std::clamp((outputs.roll - 0x8000) / 32767.0f * strength, -1.0f, 1.0f) : 0.0f;
  if (d.haptic && (d.spring >= 0 || d.friction >= 0 || d.torque >= 0)) {
    // Only what changed: the road's roll changes most frames, the spring and
    // friction seldom, and each update is a report to the wheel.
    SDL_HapticEffect e;
    if (d.spring >= 0 && centering != d.sent[0]) {
      SDL_UpdateHapticEffect(d.haptic, d.spring, &(e = condition_effect(SDL_HAPTIC_SPRING, centering)));
      d.sent[0] = centering;
    }
    if (d.friction >= 0 && friction != d.sent[1]) {
      SDL_UpdateHapticEffect(d.haptic, d.friction, &(e = condition_effect(SDL_HAPTIC_FRICTION, friction)));
      d.sent[1] = friction;
    }
    if (d.torque >= 0 && roll != d.sent[2]) {
      SDL_UpdateHapticEffect(d.haptic, d.torque, &(e = constant_effect(roll)));
      d.sent[2] = roll;
    }
    return;
  }
  if (d.gamepad && config.rumble) {
    // The road's buzz is too faint to feel; bumps and hits are not.
    const auto amount = static_cast<Uint16>(std::clamp(std::fabs(roll) * 2.0f, 0.0f, 1.0f) * 65535.0f);
    SDL_RumbleGamepad(d.gamepad, amount, amount / 2, amount ? 250 : 0);
  }
}

void HostInput::set_outputs(const triforce::Outputs& outputs) {
  Impl& m = *impl_;
  const triforce::Outputs previous = m.outputs;
  m.outputs = outputs;
  const bool changed = previous.centering != outputs.centering || previous.friction != outputs.friction ||
                       previous.roll != outputs.roll;
  // Rumble runs out by itself, so it is renewed while it lasts.
  const auto now = std::chrono::steady_clock::now();
  const bool renew = now - m.last_play > std::chrono::milliseconds(150);
  if (!changed && !renew) return;
  m.last_play = now;
  for (auto& [id, d] : m.devices) {
    if (changed || (d.gamepad && !d.haptic)) m.play(d);
  }
}

triforce::Controls HostInput::poll() {
  Impl& m = *impl_;
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::min(std::chrono::duration<float>(now - m.last_poll).count(), 0.1f);
  m.last_poll = now;

  std::array<float, kControlCount> amount{};  // strongest input for each control
  float steer_axis = 0.0f;
  // With a steering wheel connected, the joysticks' steering comes from
  // wheels only: the same [joystick] bindings apply to every joystick, and a
  // separate set of pedals, a handbrake or a throttle could otherwise turn
  // the wheel by its own axis 0.
  bool have_wheel = false;
  for (const auto& [id, d] : m.devices) have_wheel |= !d.gamepad && d.wheel;
  auto gather = [&](const Bindings& b, const Impl::Device* d) {
    for (std::size_t c = 0; c < kControlCount; ++c) {
      if (c == kPause && !d) continue;  // pause keys: see handle_event()
      for (const Source& s : b.sources[c]) {
        if (!Impl::present(s, d)) continue;
        if (c == kSteer) {
          if (!Impl::is_axis(s)) continue;
          if (d && !d->gamepad && have_wheel && !d->wheel) continue;
          const float v = Impl::axis_value(s, d, b.deadzone);
          if (std::fabs(v) > std::fabs(steer_axis)) steer_axis = v;
        } else {
          amount[c] = std::max(amount[c], Impl::amount(s, d, c == kGas || c == kBrake ? b.deadzone : 0.0f));
        }
      }
    }
  };
  gather(m.config.keyboard, nullptr);
  for (const auto& [id, d] : m.devices) gather(d.gamepad ? m.config.gamepad : m.config.joystick, &d);

  // Digital steering (and half axes) turns gradually, and recentres faster.
  const float target = amount[kSteerRight] - amount[kSteerLeft];
  const float step = m.config.steer_speed * dt * ((target == 0.0f || target * m.digital_steer < 0.0f) ? 2.0f : 1.0f);
  if (m.digital_steer < target) m.digital_steer = std::min(m.digital_steer + step, target);
  else m.digital_steer = std::max(m.digital_steer - step, target);
  const float steer = std::clamp(steer_axis + m.digital_steer, -1.0f, 1.0f);

  triforce::Controls c;
  c.steering = steer;
  c.gas = amount[kGas];
  c.brake = amount[kBrake];
  c.item = amount[kItem] > 0.5f;
  c.cancel = amount[kCancel] > 0.5f;
  c.start = amount[kStart] > 0.5f;
  c.coin = amount[kCoin] > 0.5f;
  c.test = amount[kTest] > 0.5f;
  c.service = amount[kService] > 0.5f;

  // Back + Start opens the menu (see menu_chord.h): the first of the two is
  // held back from the game for a moment.
  bool half = false, both = false;
  for (const auto& [id, d] : m.devices) {
    if (!d.gamepad) continue;
    const bool back = SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_BACK);
    const bool start = SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_START);
    both |= back && start;
    half |= back != start;
  }
  m.menu_chord.filter(half, both, now, &c);

  const bool pause = amount[kPause] > 0.5f;
  if (pause && !m.pause_held) m.pause_pressed = true;
  m.pause_held = pause;

  // The profiler's actions, from held keys, buttons and axes. Like the pause
  // control, their quick key and button presses are also caught by
  // handle_event(); here they are seen as edges, so holding one does not
  // repeat it. While an input is being chosen for a control (the menu's
  // capture), what is pressed is for the capture: no action.
  u32 profiler = 0;
  for (std::size_t c = kProfilerFirst; c < kControlCount; ++c)
    if (amount[c] > 0.5f) profiler |= 1u << (c - kProfilerFirst);
  if (!m.capture.active) m.profiler_pressed |= profiler & ~m.profiler_held;
  m.profiler_held = profiler;
  return c;
}

u32 HostInput::take_profiler_actions() {
  const u32 actions = impl_->profiler_pressed;
  impl_->profiler_pressed = 0;
  return actions;
}

bool HostInput::take_pause_press() {
  const bool pressed = impl_->pause_pressed;
  impl_->pause_pressed = false;
  return pressed;
}

}  // namespace input

namespace input {

// ---- Seeing and changing the bindings ------------------------------------------------------

const char* HostInput::control_name(std::size_t control) { return control < kControlCount ? kControlNames[control] : ""; }
const char* HostInput::control_label(std::size_t control) { return control < kControlCount ? kControlLabels[control] : ""; }
bool HostInput::control_is_axis(std::size_t control) { return control == kSteer; }

std::vector<std::string> HostInput::bindings(DeviceKind kind, std::size_t control) const {
  std::vector<std::string> names;
  if (control >= kControlCount) return names;
  for (const Source& source : impl_->bindings(kind).sources[control]) names.push_back(source_name(source));
  return names;
}

std::vector<std::string> HostInput::binding_labels(DeviceKind kind, std::size_t control) const {
  std::vector<std::string> names;
  if (control >= kControlCount) return names;
  for (const Source& source : impl_->bindings(kind).sources[control]) {
    std::string name;
    if (source.kind == Source::Kind::Key) {
      // As in key events: Latin letters on non-Latin layouts and the digits on
      // a French number row (SDL_HINT_KEYCODE_OPTIONS), which is what the key
      // caps of a game's controls are usually called by.
      const SDL_Keycode key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(source.index), SDL_KMOD_NONE, true);
      if (key == SDLK_RETURN) name = "Enter";  // as most keyboards print it (SDL says Return)
      else if (const char* n = SDL_GetKeyName(key)) name = n;
    }
    names.push_back(name.empty() ? source_name(source) : name);
  }
  return names;
}

void HostInput::remove_binding(DeviceKind kind, std::size_t control, std::size_t index) {
  if (control >= kControlCount) return;
  auto& sources = impl_->bindings(kind).sources[control];
  if (index >= sources.size()) return;
  sources.erase(sources.begin() + static_cast<std::ptrdiff_t>(index));
  impl_->dirty = true;
}

void HostInput::start_capture(DeviceKind kind, std::size_t control) {
  Impl& m = *impl_;
  m.capture = {};
  if (control >= kControlCount) return;
  m.capture.active = true;
  m.capture.kind = kind;
  m.capture.control = control;
  for (const auto& [id, d] : m.devices) {
    std::vector<int>& axes = m.capture.axes[id];
    if (d.gamepad) {
      for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) axes.push_back(SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(a)));
    } else {
      // An axis that hasn't reported yet reads 0 whatever its position: its
      // first report sets where it rests (see capture_event).
      for (int a = 0; a < SDL_GetNumJoystickAxes(d.joystick); ++a) {
        Sint16 initial = 0;
        axes.push_back(SDL_GetJoystickAxisInitialState(d.joystick, a, &initial) ? SDL_GetJoystickAxis(d.joystick, a)
                                                                                : kUnknownAxis);
      }
    }
  }
}

void HostInput::cancel_capture() { impl_->capture.active = false; }
bool HostInput::capturing() const { return impl_->capture.active; }

bool HostInput::Impl::finish_capture(const Source& source) {
  auto& sources = bindings(capture.kind).sources[capture.control];
  if (std::none_of(sources.begin(), sources.end(), [&](const Source& s) { return same_source(s, source); })) {
    sources.push_back(source);
    dirty = true;
  }
  capture.active = false;
  return true;
}

bool HostInput::capture_event(const SDL_Event& e) {
  Impl& m = *impl_;
  if (!m.capture.active) return false;
  const DeviceKind kind = m.capture.kind;
  const bool steering = control_is_axis(m.capture.control);
  // How far an axis must move from where it was, and where a pedal rests.
  constexpr int kMoved = 16000, kRest = 24000;
  auto baseline = [&](SDL_JoystickID id, int axis) -> int {
    const auto it = m.capture.axes.find(id);
    return it != m.capture.axes.end() && axis < static_cast<int>(it->second.size()) ? it->second[axis] : 0;
  };
  auto is_gamepad = [&](SDL_JoystickID id) {
    const auto it = m.devices.find(id);
    return it != m.devices.end() && it->second.gamepad;
  };
  Source s;
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
      if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
        m.capture.active = false;
        return true;
      }
      if (kind != DeviceKind::Keyboard || steering || e.key.repeat) return true;
      // A key SDL has no name for could not be saved: keep waiting.
      if (e.key.scancode == SDL_SCANCODE_UNKNOWN || !*SDL_GetScancodeName(e.key.scancode)) return true;
      s.kind = Source::Kind::Key;
      s.index = e.key.scancode;
      return m.finish_capture(s);
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT: return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      if (kind != DeviceKind::Gamepad || steering) return true;
      s.kind = Source::Kind::PadButton;
      s.index = e.gbutton.button;
      return m.finish_capture(s);
    case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      if (kind != DeviceKind::Gamepad) return true;
      const int moved = e.gaxis.value - baseline(e.gaxis.which, e.gaxis.axis);
      if (std::abs(moved) < kMoved) return true;
      s.kind = Source::Kind::PadAxis;
      s.index = e.gaxis.axis;
      if (!steering && !is_trigger(s.index)) s.range = moved > 0 ? Source::Range::Positive : Source::Range::Negative;
      if (steering && is_trigger(s.index)) return true;
      return m.finish_capture(s);
    }
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
      if (kind != DeviceKind::Joystick || steering || is_gamepad(e.jbutton.which)) return kind == DeviceKind::Joystick;
      s.kind = Source::Kind::JoyButton;
      s.index = e.jbutton.button;
      return m.finish_capture(s);
    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
      if (kind != DeviceKind::Joystick || steering || is_gamepad(e.jhat.which)) return kind == DeviceKind::Joystick;
      const u8 v = e.jhat.value;
      if (v != SDL_HAT_UP && v != SDL_HAT_DOWN && v != SDL_HAT_LEFT && v != SDL_HAT_RIGHT) return true;
      s.kind = Source::Kind::JoyHat;
      s.index = e.jhat.hat;
      s.hat = v;
      return m.finish_capture(s);
    }
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
      if (kind != DeviceKind::Joystick || is_gamepad(e.jaxis.which)) return kind == DeviceKind::Joystick;
      if (baseline(e.jaxis.which, e.jaxis.axis) == kUnknownAxis) {
        // The axis's first report: where it rests, not a movement.
        m.capture.axes[e.jaxis.which][e.jaxis.axis] = e.jaxis.value;
        return true;
      }
      const int base = baseline(e.jaxis.which, e.jaxis.axis);
      const int moved = e.jaxis.value - base;
      if (std::abs(moved) < kMoved) return true;
      s.kind = Source::Kind::JoyAxis;
      s.index = e.jaxis.axis;
      if (!steering) {
        // A pedal resting at one end is a whole axis (turned around if it
        // rests at the top); an axis resting in the middle, half of one.
        if (base < -kRest) s.range = Source::Range::Whole;
        else if (base > kRest) s.invert = true;
        else s.range = moved > 0 ? Source::Range::Positive : Source::Range::Negative;
      }
      return m.finish_capture(s);
    }
    default: return false;
  }
}

float HostInput::force_feedback() const { return impl_->config.ffb_strength; }
void HostInput::set_force_feedback(float strength) {
  if (!std::isfinite(strength)) return;
  strength = std::clamp(strength, 0.0f, 2.0f);
  if (strength == impl_->config.ffb_strength) return;
  impl_->config.ffb_strength = strength;
  impl_->dirty = true;
  for (auto& [id, d] : impl_->devices) impl_->play(d);
}

float HostInput::steer_speed() const { return impl_->config.steer_speed; }
void HostInput::set_steer_speed(float speed) {
  if (!std::isfinite(speed)) return;
  speed = std::max(0.1f, speed);
  if (speed == impl_->config.steer_speed) return;
  impl_->config.steer_speed = speed;
  impl_->dirty = true;
}

bool HostInput::background_input() const { return impl_->config.background; }
void HostInput::set_background_input(bool on) {
  if (on == impl_->config.background) return;
  impl_->config.background = on;
  impl_->apply_background();
  impl_->dirty = true;
}

bool HostInput::background_keyboard() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

float HostInput::deadzone(DeviceKind kind) const { return impl_->bindings(kind).deadzone; }
void HostInput::set_deadzone(DeviceKind kind, float deadzone) {
  if (!std::isfinite(deadzone)) return;
  deadzone = std::clamp(deadzone, 0.0f, 0.9f);
  if (deadzone == impl_->bindings(kind).deadzone) return;
  impl_->bindings(kind).deadzone = deadzone;
  impl_->dirty = true;
}

void HostInput::restore_defaults() {
  Config defaults;
  std::string why;
  parse_config(kDefaultConfig, &defaults, &why);
  impl_->config = defaults;
  impl_->apply_background();
  impl_->dirty = true;
}

bool HostInput::save(std::string* error) {
  Impl& m = *impl_;
  if (!m.dirty || m.path.empty() || m.keep_file) return true;
  const std::string text = config_text(m.config);
  if (!os::write_file_atomically(m.path, text.data(), text.size(), error)) return false;
  m.dirty = false;
  return true;
}

const std::string& HostInput::path() const { return impl_->path; }

}  // namespace input
