// SPDX-License-Identifier: GPL-3.0-or-later
//
// Discord's local connection ("IPC"), which Rich Presence uses. The Discord
// app on the player's machine listens on a named pipe (Windows:
// \\.\pipe\discord-ipc-0 to -9) or a Unix socket (discord-ipc-0 to -9 in the
// runtime or temporary directory, or in a Flatpak's or Snap's), and programs
// talk to it in messages: a little-endian opcode and length, then that many
// bytes of JSON. That is all OpenGP needs, so it does not use Discord's SDK.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/types.h"

namespace discord {

using namespace gekko;

enum class Opcode : u32 {
  Handshake = 0,  // {"v":1,"client_id":"..."}; Discord answers with a READY event
  Frame = 1,      // commands and their answers
  Close = 2,      // either side closes (Discord says why)
  Ping = 3,
  Pong = 4,
};

struct Message {
  Opcode op = Opcode::Frame;
  std::string json;
};

// A connection to Discord (or a stand-in, for tests).
class Transport {
 public:
  virtual ~Transport() = default;
  // False once the connection has failed or been closed.
  virtual bool connected() const = 0;
  virtual void write(const Message& message) = 0;
  // The next message, if one arrives within `timeout_ms`.
  virtual std::optional<Message> read(int timeout_ms) = 0;
};

// Connects to the Discord app running on this machine; null if none is.
std::unique_ptr<Transport> connect_local();

// A message as bytes on the connection.
std::vector<u8> encode(const Message& message);

// Takes the first whole message off the front of `bytes`, if there is one.
// Sets `bad` (and returns nothing) if the bytes cannot be a message.
std::optional<Message> decode(std::vector<u8>& bytes, bool* bad);

}  // namespace discord
