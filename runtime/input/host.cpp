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

#include "os/atomic_file.h"

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
  kControlCount
};
constexpr std::array<const char*, kControlCount> kControlNames = {
    "steer", "steer_left", "steer_right", "gas", "brake", "item", "cancel", "start", "coin", "test", "service"};
constexpr std::array<const char*, kControlCount> kControlLabels = {
    "Steering", "Steer left", "Steer right", "Gas", "Brake", "Item button", "Cancel button",
    "Start",    "Coin",       "Test",        "Service"};
static_assert(kControlCount == HostInput::kControlCount);

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
};

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
    auto add = [&](const toml::node& item) {
      const auto name = item.value<std::string>();
      Source s;
      if (!name || !parse_source(*name, device, &s)) {
        *error = std::string("[") + section + "] " + kControlNames[c] + ": unknown input \"" +
                 (name ? *name : std::string("?")) + "\"";
        return false;
      }
      sources.push_back(s);
      return true;
    };
    if (const toml::array* list = node->as_array()) {
      for (const toml::node& item : *list) {
        if (!add(item)) return false;
      }
    } else if (!add(*node)) {
      return false;
    }
    out->sources[c] = std::move(sources);
  }
  if (const auto deadzone = (*table)["deadzone"].value<double>()) {
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
  c.steer_speed = std::max(0.1f, static_cast<float>(root["steering"]["speed"].value_or(double{c.steer_speed})));
  c.ffb_strength =
      std::clamp(static_cast<float>(root["force_feedback"]["strength"].value_or(double{c.ffb_strength})), 0.0f, 2.0f);
  c.rumble = root["force_feedback"]["rumble"].value_or(c.rumble);
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
      for (std::size_t i = 0; i < b.sources[control].size(); ++i) {
        std::string name = source_name(b.sources[control][i]);
        std::string quoted = "\"";
        for (const char ch : name) {
          if (ch == '"' || ch == '\\') quoted += '\\';
          quoted += ch;
        }
        out += (i ? ", " : "") + quoted + "\"";
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
         "\n# Gamepads rumble on bumps and hits.\nrumble = " + (c.rumble ? "true" : "false") + "\n";
  return out;
}

float apply_deadzone(float v, float deadzone) {
  const float m = std::fabs(v);
  if (m <= deadzone) return 0.0f;
  return std::copysign((m - deadzone) / (1.0f - deadzone), v);
}

}  // namespace

struct HostInput::Impl {
  struct Device {
    SDL_Gamepad* gamepad = nullptr;
    SDL_Joystick* joystick = nullptr;  // owned only without a gamepad
    SDL_Haptic* haptic = nullptr;
    // Wheel effects (-1: not supported).
    int spring = -1, friction = -1, torque = -1;
  };

  Config config;
  std::string path;
  bool dirty = false;  // bindings changed since load() or save()
  std::map<SDL_JoystickID, Device> devices;
  // Binding an input from the menu: waiting for one of `kind`, for `control`.
  struct Capture {
    bool active = false;
    DeviceKind kind = DeviceKind::Keyboard;
    std::size_t control = 0;
    std::map<SDL_JoystickID, std::vector<Sint16>> axes;  // where each axis was at the start
  } capture;
  float digital_steer = 0.0f;  // -1..1, keys and buttons turn it gradually
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
  // As a whole axis (steering): -1..1.
  static float axis_value(const Source& s, const Device* d, float deadzone) {
    float v = raw(s, d);
    if (s.invert) v = -v;
    return apply_deadzone(v, deadzone);
  }
  // As a pedal or button: 0..1.
  static float amount(const Source& s, const Device* d, float deadzone) {
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
  if (!fs::exists(path, ec)) {
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream(path, std::ios::binary) << kDefaultConfig;
  }
  std::string text = kDefaultConfig;
  if (std::ifstream f(path, std::ios::binary); f) {
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::string why;
  impl_->config = Config{};
  parse_config(kDefaultConfig, &impl_->config, &why);
  if (!parse_config(text, &impl_->config, &why)) {
    if (error) *error = path + ": " + why + " (using the default controls)";
    return false;
  }
  return true;
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
    SDL_HapticEffect e;
    if (d.spring >= 0) SDL_UpdateHapticEffect(d.haptic, d.spring, &(e = condition_effect(SDL_HAPTIC_SPRING, centering)));
    if (d.friction >= 0) SDL_UpdateHapticEffect(d.haptic, d.friction, &(e = condition_effect(SDL_HAPTIC_FRICTION, friction)));
    if (d.torque >= 0) SDL_UpdateHapticEffect(d.haptic, d.torque, &(e = constant_effect(roll)));
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
  auto gather = [&](const Bindings& b, const Impl::Device* d) {
    for (std::size_t c = 0; c < kControlCount; ++c) {
      for (const Source& s : b.sources[c]) {
        if (c == kSteer) {
          if (!Impl::is_axis(s)) continue;
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
  c.steering = static_cast<u8>(std::lround(128.0f + steer * 127.0f));
  c.gas = static_cast<u8>(std::lround(amount[kGas] * 255.0f));
  c.brake = static_cast<u8>(std::lround(amount[kBrake] * 255.0f));
  c.item = amount[kItem] > 0.5f;
  c.cancel = amount[kCancel] > 0.5f;
  c.start = amount[kStart] > 0.5f;
  c.coin = amount[kCoin] > 0.5f;
  c.test = amount[kTest] > 0.5f;
  c.service = amount[kService] > 0.5f;
  return c;
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
    std::vector<Sint16>& axes = m.capture.axes[id];
    if (d.gamepad) {
      for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) axes.push_back(SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(a)));
    } else {
      for (int a = 0; a < SDL_GetNumJoystickAxes(d.joystick); ++a) axes.push_back(SDL_GetJoystickAxis(d.joystick, a));
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
  strength = std::clamp(strength, 0.0f, 2.0f);
  if (strength == impl_->config.ffb_strength) return;
  impl_->config.ffb_strength = strength;
  impl_->dirty = true;
  for (auto& [id, d] : impl_->devices) impl_->play(d);
}

float HostInput::steer_speed() const { return impl_->config.steer_speed; }
void HostInput::set_steer_speed(float speed) {
  speed = std::max(0.1f, speed);
  if (speed == impl_->config.steer_speed) return;
  impl_->config.steer_speed = speed;
  impl_->dirty = true;
}

float HostInput::deadzone(DeviceKind kind) const { return impl_->bindings(kind).deadzone; }
void HostInput::set_deadzone(DeviceKind kind, float deadzone) {
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
  impl_->dirty = true;
}

bool HostInput::save(std::string* error) {
  Impl& m = *impl_;
  if (!m.dirty || m.path.empty()) return true;
  const std::string text = config_text(m.config);
  if (!os::write_file_atomically(m.path, text.data(), text.size())) {
    if (error) *error = "cannot write " + m.path;
    return false;
  }
  m.dirty = false;
  return true;
}

const std::string& HostInput::path() const { return impl_->path; }

}  // namespace input
