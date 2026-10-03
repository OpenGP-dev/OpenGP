// SPDX-License-Identifier: GPL-3.0-or-later
//
// Magnetic card reader/writer/printer (Sanwa CRP-1231LR, as used by Mario
// Kart Arcade GP and GP 2 for save cards). A card is kept as a file of up to
// three 69-byte tracks (see card_folder.h); a simulated player inserts the
// card they hold when the game asks.
// Based on Dolphin's MagCard/MagneticCardReader.cpp (Copyright Dolphin
// Emulator Project, GPL-2.0-or-later), itself based on YACardEmu
// (https://github.com/GXTX/YACardEmu, Copyright (C) 2020-2023 wutno,
// GPL-2.0-or-later).
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "triforce/card_folder.h"
#include "triforce/serial_device.h"

namespace triforce {

class MagCardReader final : public SerialDevice {
 public:
  static constexpr std::size_t kTrackSize = 69;
  static constexpr std::size_t kTracks = 3;

  explicit MagCardReader(std::shared_ptr<CardFolder> cards);
  ~MagCardReader() override;
  void update() override;

 private:
  using Duration = std::chrono::milliseconds;

  // Card position ("R"), error ("P") and job ("S") status bytes.
  enum class R : u8 { NoCard, ReadWriteHead, ThermalHead, DispenserThermal, EntrySlot };
  enum class P : u8 { NoError = 0x30, ReadError = 0x31, WriteError = 0x32, PrintError = 0x35, IllegalError = 0x38, SystemError = 0x41 };
  enum class S : u8 {
    NoJob = 0x30,
    IllegalCommand = 0x32,
    RunningCommand = 0x33,
    WaitingForCard = 0x34,
    DispenserEmpty = 0x35,
    CardFull = 0x37,
  };
  struct Status {
    R r = R::NoCard;
    P p = P::NoError;
    S s = S::NoJob;
    void soft_reset() { p = P::NoError, s = S::NoJob; }
  };

  bool receive_packet(std::span<const u8> packet);
  void build_packet();
  void step_machine(Duration elapsed);
  void step_person(Duration elapsed);
  u8 position_value() const;

  void set_p_error(P error);
  void set_s_error(S error);
  void finish_command();
  bool running_command() const { return step_ > 0; }
  bool card_present() const { return status_.r != R::NoCard; }
  bool card_loaded() const { return status_.r != R::NoCard && status_.r != R::EntrySlot; }
  void read_card_file();
  void write_card_file();
  void move_card(R position);
  void eject_card();
  void dispense_card();

  void command_read_data();
  void command_get_data();
  void command_write_data();
  void command_print_line_or_erase(bool erase);
  void command_dispense();

  std::shared_ptr<CardFolder> cards_;
  std::string card_path_;  // of the card in the reader (or its entry slot)
  Status status_{};
  u8 command_ = 0;             // sent in every ENQUIRY response
  std::vector<u8> payload_;    // data sent with the ENQUIRY response
  std::vector<u8> parameters_; // parameters of the running command
  int step_ = 0;               // progress of the running command; 0: none
  std::array<std::optional<std::array<u8, kTrackSize>>, kTracks> tracks_;
  Duration since_machine_moved_card_{};
  Duration since_person_moved_card_{};
};

}  // namespace triforce
