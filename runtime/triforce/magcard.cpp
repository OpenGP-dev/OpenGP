// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/magcard.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <string_view>

#include "os/atomic_file.h"

namespace triforce {

namespace {

constexpr std::string_view kVersion = "AP:S1234-5678,OS:S9012-3456,0000";

constexpr u8 STX = 0x02;  // start of a command packet
constexpr u8 ETX = 0x03;
constexpr u8 ENQ = 0x05;  // the host polls for the status packet
constexpr u8 ACK = 0x06;
constexpr u8 NAK = 0x15;

// Track selection parameter: 0x30-0x32 one track, 0x33-0x35 two, 0x36 all three.
std::vector<std::size_t> track_indices(u8 track) {
  switch (track) {
    case 0x30:
    case 0x31:
    case 0x32: return {static_cast<std::size_t>(track - 0x30)};
    case 0x33: return {0, 1};
    case 0x34: return {0, 2};
    case 0x35: return {1, 2};
    case 0x36: return {0, 1, 2};
    default: return {};
  }
}

void card_message(const char* text) { std::fprintf(stderr, "[card] %s\n", text); }

}  // namespace

MagCardReader::MagCardReader(std::shared_ptr<CardFolder> cards) : cards_(std::move(cards)) {}

MagCardReader::~MagCardReader() {
  if (card_loaded()) card_message("shut down with a card in the reader");
}

u8 MagCardReader::position_value() const {
  // CRP-1231LR position codes.
  static constexpr u8 kValues[] = {0x30, 0x31, 0x32, 0x33, 0x34};
  return kValues[static_cast<std::size_t>(status_.r)];
}

void MagCardReader::read_card_file() {
  for (auto& t : tracks_) t.reset();
  std::ifstream file(os::utf8_path(card_path_), std::ios::binary);
  for (auto& t : tracks_) {
    std::array<u8, kTrackSize> data{};
    if (!file.read(reinterpret_cast<char*>(data.data()), kTrackSize)) break;
    t = data;
  }
}

void MagCardReader::write_card_file() {
  // Only the game writes cards: creating blank cards here would upset games,
  // which expect to dispense their own. The tracks written replace those in
  // the file; the whole file is rewritten, through a temporary file.
  std::string data;
  if (std::ifstream file(os::utf8_path(card_path_), std::ios::binary); file) {
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  for (std::size_t i = 0; i < kTracks; ++i) {
    if (!tracks_[i]) continue;
    if (data.size() < (i + 1) * kTrackSize) data.resize((i + 1) * kTrackSize);
    std::copy(tracks_[i]->begin(), tracks_[i]->end(), data.begin() + static_cast<std::ptrdiff_t>(i * kTrackSize));
  }
  if (!os::write_file_atomically(card_path_, data.data(), data.size())) card_message("writing the card file failed");
}

void MagCardReader::update() {
  for (;;) {
    const auto data = rx();
    if (data.empty()) break;
    if (data[0] == ENQ) {
      // The host polls regularly; each poll advances the running command
      // (assumed to be 50 ms apart) and returns the status packet.
      step_machine(Duration{50});
      step_person(Duration{50});
      build_packet();
      consume_rx(1);
      continue;
    }
    if (data[0] != STX) {
      consume_rx(1);
      continue;
    }
    if (data.size() < 2) break;
    const std::size_t size = data[1];
    if (size > data.size() - 2) break;
    write_tx(receive_packet(data.subspan(2, size)) ? ACK : NAK);
    consume_rx(size + 2);
  }
}

bool MagCardReader::receive_packet(std::span<const u8> packet) {
  // command, 3 host status bytes, parameters, ETX, checksum
  if (packet.size() < 6 || packet[packet.size() - 2] != ETX) return false;
  // XOR of all bytes including the (already removed) count byte.
  if (std::accumulate(packet.begin(), packet.end(), static_cast<u8>(packet.size()), std::bit_xor<u8>{}) != 0) return false;
  command_ = packet[0];
  parameters_.assign(packet.begin() + 4, packet.end() - 2);
  status_.soft_reset();
  status_.s = S::RunningCommand;
  step_ = 1;
  payload_.clear();
  return true;
}

void MagCardReader::build_packet() {
  const u8 lead_in[] = {static_cast<u8>(payload_.size() + 6), command_, position_value(), static_cast<u8>(status_.p),
                        static_cast<u8>(status_.s)};
  write_tx(STX);  // not part of the checksum
  write_tx(lead_in);
  write_tx(payload_);
  write_tx(ETX);
  u8 checksum = ETX;
  for (const u8 v : lead_in) checksum ^= v;
  for (const u8 v : payload_) checksum ^= v;
  write_tx(checksum);
}

void MagCardReader::step_person(Duration elapsed) {
  // Emulates a player: insert the card they hold into the entry slot when
  // the reader is empty, take it back once the machine has ejected it (or if
  // they chose another card meanwhile).
  // Mario Kart GP polls for cards with ReadData(CardCapture) whenever it
  // wants one, and waits for ejected cards to be taken.
  since_person_moved_card_ += elapsed;
  cards_->set_inserted(card_loaded());
  if (card_loaded()) return;
  const std::string held = cards_->held();
  const bool have_card = cards_->exists(held);
  if (!card_present()) {
    if (since_person_moved_card_ > Duration{500} && have_card) {
      card_path_ = cards_->path(held);
      status_.r = R::EntrySlot;
      since_person_moved_card_ = {};
    }
    return;
  }
  const bool machine_moved_last = since_machine_moved_card_ < since_person_moved_card_;
  if ((machine_moved_last && since_machine_moved_card_ > Duration{100}) || !have_card ||
      cards_->path(held) != card_path_) {
    status_.r = R::NoCard;
    since_person_moved_card_ = {};
  }
}

void MagCardReader::step_machine(Duration elapsed) {
  since_machine_moved_card_ += elapsed;
  if (!running_command()) return;
  switch (command_) {
    case 0x10:  // initialize
      eject_card();
      status_.soft_reset();
      finish_command();
      break;
    case 0x20:  // read status
    case 0x40:  // cancel
    case 0x78:  // print settings
    case 0x7A:  // register font
    case 0xC0:  // LED control
    case 0xC1:  // print retry count
      status_.soft_reset();
      finish_command();
      break;
    case 0x33: command_read_data(); break;
    case 0x35: command_get_data(); break;
    case 0x53: command_write_data(); break;
    case 0x7B:  // print image
      if (!card_present()) return set_p_error(P::PrintError);
      status_.soft_reset();
      finish_command();
      break;
    case 0x7C: command_print_line_or_erase(false); break;
    case 0x7D: command_print_line_or_erase(true); break;
    case 0x7E:  // print barcode
      if (step_ == 2 && !card_present()) return set_p_error(P::IllegalError);
      if (step_ >= 2) finish_command();
      break;
    case 0x80:  // eject
      if (step_ >= 3) {
        eject_card();
        finish_command();
      }
      break;
    case 0xA0:  // clean
      if (step_ == 2 && !card_loaded()) {
        card_message("inserting a cleaning card");
        move_card(R::ThermalHead);
      }
      if (step_ == 3) eject_card();
      if (step_ >= 3) finish_command();
      break;
    case 0xB0: command_dispense(); break;
    case 0xF0:  // version
      payload_.insert(payload_.end(), kVersion.begin(), kVersion.end());
      status_.soft_reset();
      finish_command();
      break;
    default:  // 0xD0 (shutter) is only on the BR model
      std::fprintf(stderr, "[card] unhandled command %02X\n", command_);
      set_s_error(S::IllegalCommand);
      break;
  }
  if (running_command()) ++step_;
}

void MagCardReader::command_read_data() {
  if (parameters_.size() < 3) return set_p_error(P::SystemError);
  const u8 mode = parameters_[0];  // 0x30 standard, 0x31 variable length, 0x32 card capture
  if (step_ == 2) {
    if (mode == 0x32) {
      // Capture: pull the card in; no data.
      if (!card_present()) {
        status_.s = S::WaitingForCard;
        --step_;
        return;
      }
      if (status_.r != R::ReadWriteHead) {
        move_card(R::ReadWriteHead);
        read_card_file();
        std::fprintf(stderr, "[card] inserted: %s\n", card_path_.c_str());
      }
    } else {
      const auto indices = track_indices(parameters_[2]);
      if (!card_loaded() || indices.empty()) return set_p_error(P::IllegalError);
      for (const std::size_t i : indices) {
        if (!tracks_[i]) return set_p_error(P::ReadError);
        payload_.insert(payload_.end(), tracks_[i]->begin(), tracks_[i]->end());
      }
    }
  }
  if (step_ >= 2) finish_command();
}

void MagCardReader::command_get_data() {
  if (step_ == 2) {
    if (!card_loaded()) return set_p_error(P::IllegalError);
    for (const auto& t : tracks_) {
      if (!t) return set_p_error(P::ReadError);
      payload_.insert(payload_.end(), t->begin(), t->end());
    }
  }
  if (step_ >= 2) finish_command();
}

void MagCardReader::command_write_data() {
  if (parameters_.size() < 3) return set_p_error(P::SystemError);
  if (step_ < 2) return;
  const auto indices = track_indices(parameters_[2]);
  const auto data = std::span{parameters_}.subspan(3);
  if (parameters_[0] != 0x30 || !card_loaded() || indices.empty() || data.size() != indices.size() * kTrackSize) {
    card_message("write failed");
    return set_p_error(P::IllegalError);
  }
  // One 69-byte block per selected track, in track order.
  for (std::size_t n = 0; n < indices.size(); ++n) {
    std::array<u8, kTrackSize> track{};
    const auto src = data.subspan(n * kTrackSize, kTrackSize);
    std::copy(src.begin(), src.end(), track.begin());
    tracks_[indices[n]] = track;
  }
  card_message("writing");
  write_card_file();
  finish_command();
}

void MagCardReader::command_print_line_or_erase(bool erase) {
  if (!erase && parameters_.size() < 3) return set_p_error(P::SystemError);
  if (step_ == 1) {
    if (!card_present()) return set_p_error(P::PrintError);
    move_card(R::ThermalHead);
  } else if (step_ == 2) {
    move_card(R::ReadWriteHead);
  }
  if (step_ >= 2) finish_command();
}

void MagCardReader::command_dispense() {
  // Mario Kart GP issues this without parameters.
  const u8 mode = parameters_.empty() ? 0x31 : parameters_[0];  // 0x31 dispense, 0x32 check only
  if (step_ == 2) {
    if (mode == 0x32) status_.s = S::CardFull;
    else dispense_card();
  } else if (step_ == 3 && status_.r == R::DispenserThermal) {
    move_card(R::ReadWriteHead);
  }
  if (step_ >= 3) finish_command();
}

void MagCardReader::set_p_error(P error) {
  status_.p = error;
  finish_command();
}

void MagCardReader::set_s_error(S error) {
  status_.s = error;
  finish_command();
}

void MagCardReader::finish_command() {
  step_ = 0;
  if (status_.s != S::IllegalCommand) status_.s = S::NoJob;
}

void MagCardReader::move_card(R position) {
  if (status_.r == position) return;
  status_.r = position;
  since_machine_moved_card_ = {};
}

void MagCardReader::eject_card() {
  if (!card_loaded()) return;
  for (auto& t : tracks_) t.reset();
  move_card(R::EntrySlot);
  card_message("ejected");
}

void MagCardReader::dispense_card() {
  if (card_loaded()) return set_s_error(S::IllegalCommand);
  // A blank card, saved under the held card's name if that has no file yet,
  // else under a new name; the player then holds it.
  std::string name = cards_->held();
  if (name.empty() || cards_->exists(name)) name = cards_->unused_name();
  cards_->hold(name);
  card_path_ = cards_->path(name);
  for (auto& t : tracks_) t.reset();
  std::fprintf(stderr, "[card] dispensing a new card: %s\n", card_path_.c_str());
  move_card(R::DispenserThermal);
}

}  // namespace triforce
