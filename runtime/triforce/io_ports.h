// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cabinet's inputs and outputs as the JVS I/O board sees them, and the
// game-specific adapters that connect them to the player's controls.
// Based on Dolphin's Triforce/IOPorts.h (Copyright Dolphin Emulator Project,
// GPL-2.0-or-later).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "gekko/types.h"

namespace triforce {

using namespace gekko;

// What the player is doing with the cabinet, as provided by the host's input
// layer. Analog values are 8-bit (0x80 = centered steering).
struct Controls {
  bool test = false, service = false, coin = false;
  bool start = false, item = false, cancel = false;
  // At full precision: the cabinet's analog inputs are 16-bit, and a wheel
  // or pedal of 10 bits or more would lose steps at 8.
  float steering = 0.0f;           // -1 (full left) to 1 (full right)
  float gas = 0.0f, brake = 0.0f;  // 0 (released) to 1
};
using ControlsSource = std::function<Controls()>;

// What the cabinet does to the player: steering wheel force feedback and the
// button lamps. Forces are the game's raw values.
struct Outputs {
  u16 centering = 0;  // pull towards the centre, 0..0x7FFF
  u16 friction = 0;   // resistance to turning, 0..0x7FFF
  u16 roll = 0;       // push to one side, 0x8000 +/- 0x7FFF (the road, bumps, hits); 0 when off
  bool item_lamp = false, cancel_lamp = false;
  bool operator==(const Outputs&) const = default;
};
using OutputsSink = std::function<void(const Outputs&)>;

class IOAdapter;

class IOPorts {
 public:
  static constexpr std::size_t kPlayers = 2;
  static constexpr std::size_t kCoinSlots = 2;
  static constexpr std::size_t kAnalogInputs = 8;
  static constexpr std::size_t kGenericOutputBytes = 4;
  static constexpr u16 kNeutralAnalog = 0x8000;

  IOPorts();
  ~IOPorts();

  // Resets the inputs and lets the adapters fill them in.
  void update();

  std::span<u8> status_switches() { return status_switches_; }
  u8& system_inputs() { return system_inputs_; }
  std::span<u8> switch_inputs(std::size_t player) { return std::span{switch_inputs_}.subspan(kSwitchBytesPerPlayer * player, kSwitchBytesPerPlayer); }
  std::span<u16> analog_inputs() { return analog_inputs_; }
  std::span<bool> coin_inputs() { return coin_inputs_; }

  std::span<const u8> generic_outputs() const { return generic_outputs_; }
  void set_generic_outputs(std::span<const u8> bytes);
  void reset_generic_outputs();

  void add_adapter(std::unique_ptr<IOAdapter> adapter);

 private:
  static constexpr std::size_t kSwitchBytesPerPlayer = 2;

  std::vector<std::unique_ptr<IOAdapter>> adapters_;
  std::array<u8, 2> status_switches_{0xFF, 0xFF};
  u8 system_inputs_ = 0;  // the first byte of JVS switch input data
  std::array<u8, kPlayers * kSwitchBytesPerPlayer> switch_inputs_{};
  std::array<u16, kAnalogInputs> analog_inputs_{};
  std::array<u8, kGenericOutputBytes> generic_outputs_{};
  std::array<bool, kCoinSlots> coin_inputs_{};
};

class IOAdapter {
 public:
  virtual ~IOAdapter() = default;
  virtual void update(IOPorts& ports) { (void)ports; }
  // Called with the generic output bits that were just set and cleared.
  virtual void generic_outputs_changed(std::span<const u8> bits_set, std::span<const u8> bits_cleared) {
    (void)bits_set, (void)bits_cleared;
  }
};

// Test, service and coin, which every cabinet has.
class CommonIOAdapter final : public IOAdapter {
 public:
  explicit CommonIOAdapter(ControlsSource controls) : controls_(std::move(controls)) {}
  void update(IOPorts& ports) override;

 private:
  ControlsSource controls_;
};

// An 8-bit analog value as the I/O board reports it (16 bits, low bit repeated).
constexpr u16 expand_analog(u8 value) { return static_cast<u16>((value << 8) | ((value & 1) ? 0xFF : 0x00)); }

}  // namespace triforce
