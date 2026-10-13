// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for Discord Rich Presence: the local connection's messages
// (runtime/discord/ipc.h), the commands and the client (discord/presence.h)
// against a stand-in for Discord, and what the games' status shows
// (app/rich_presence.h).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "app/rich_presence.h"
#include "discord/ipc.h"
#include "discord/presence.h"

using discord::Message;
using discord::Opcode;

namespace {

// Discord, as far as Rich Presence sees it: answers the handshake (or turns
// it down) and records what it is sent.
struct FakeDiscord {
  std::mutex mutex;
  std::vector<Message> received;
  int connections = 0;
  bool accept = true;   // false: closes the handshake, as for an unknown application
  bool hang_up = false;  // closes the current connection

  std::vector<Message> messages() {
    std::lock_guard lock(mutex);
    return received;
  }
  std::vector<std::string> activities() {
    std::vector<std::string> out;
    for (const Message& m : messages())
      if (m.op == Opcode::Frame) out.push_back(m.json);
    return out;
  }
};

class FakeTransport : public discord::Transport {
 public:
  explicit FakeTransport(FakeDiscord& d) : d_(d) {}

  bool connected() const override {
    std::lock_guard lock(d_.mutex);
    return open_ && !d_.hang_up;
  }

  void write(const Message& message) override {
    std::lock_guard lock(d_.mutex);
    d_.received.push_back(message);
    if (message.op == Opcode::Handshake) {
      if (d_.accept) replies_.push_back({Opcode::Frame, R"({"cmd":"DISPATCH","data":{"v":1},"evt":"READY"})"});
      else replies_.push_back({Opcode::Close, R"({"code":4000,"message":"Invalid Client ID"})"});
    }
  }

  std::optional<Message> read(int timeout_ms) override {
    {
      std::lock_guard lock(d_.mutex);
      if (!replies_.empty()) {
        Message m = replies_.front();
        replies_.pop_front();
        if (m.op == Opcode::Close) open_ = false;
        return m;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
    return std::nullopt;
  }

 private:
  FakeDiscord& d_;
  std::deque<Message> replies_;
  bool open_ = true;
};

discord::Presence::Options fake_options(FakeDiscord& d) {
  discord::Presence::Options o;
  o.connect = [&d]() -> std::unique_ptr<discord::Transport> {
    std::lock_guard lock(d.mutex);
    ++d.connections;
    d.hang_up = false;
    return std::make_unique<FakeTransport>(d);
  };
  o.retry_ms = 50;
  o.min_interval_ms = 300;
  o.log = false;
  return o;
}

// Waits up to two seconds for `done`.
template <typename F>
bool eventually(F done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return done();
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("discord: messages on the connection") {
  const Message m{Opcode::Frame, R"({"a":1})"};
  std::vector<gekko::u8> bytes = discord::encode(m);
  REQUIRE(bytes.size() == 8 + 7);
  CHECK(bytes[0] == 1);  // the opcode and the length, little-endian
  CHECK(bytes[4] == 7);
  // Two messages back to back, arriving a byte at a time.
  const std::vector<gekko::u8> second = discord::encode({Opcode::Ping, "{}"});
  bytes.insert(bytes.end(), second.begin(), second.end());
  std::vector<gekko::u8> in;
  std::vector<Message> out;
  bool bad = false;
  for (const gekko::u8 b : bytes) {
    in.push_back(b);
    while (auto message = discord::decode(in, &bad)) out.push_back(*message);
  }
  CHECK_FALSE(bad);
  REQUIRE(out.size() == 2);
  CHECK(out[0].op == Opcode::Frame);
  CHECK(out[0].json == m.json);
  CHECK(out[1].op == Opcode::Ping);
  CHECK(in.empty());
  // An unknown opcode, or a length past Discord's limit.
  std::vector<gekko::u8> junk = {9, 0, 0, 0, 0, 0, 0, 0};
  CHECK_FALSE(discord::decode(junk, &bad));
  CHECK(bad);
}

TEST_CASE("discord: the commands") {
  CHECK(discord::handshake_json("123") == R"({"v":1,"client_id":"123"})");
  discord::Activity a;
  a.details = "Yoshi Cup · Round 1";
  a.state = "Say \"hi\"\\";
  a.start = 1700000000;
  a.large_image = "cup_yoshi";
  a.small_image = "mario";
  a.small_text = "Mario";
  CHECK(discord::set_activity_json(a, 42, "7") ==
        R"({"cmd":"SET_ACTIVITY","args":{"pid":42,"activity":{"details":"Yoshi Cup · Round 1",)"
        R"("state":"Say \"hi\"\\","timestamps":{"start":1700000000},)"
        R"("assets":{"large_image":"cup_yoshi","small_image":"mario","small_text":"Mario"}}},"nonce":"7"})");
  // No activity clears it; empty fields are left out.
  CHECK(discord::set_activity_json(std::nullopt, 42, "8") == R"({"cmd":"SET_ACTIVITY","args":{"pid":42},"nonce":"8"})");
  discord::Activity plain;
  plain.details = "A\nB";
  CHECK(discord::set_activity_json(plain, 1, "9") ==
        R"({"cmd":"SET_ACTIVITY","args":{"pid":1,"activity":{"details":"A\nB"}},"nonce":"9"})");
}

TEST_CASE("discord: the client shows the latest activity, no faster than allowed") {
  FakeDiscord d;
  discord::Presence p("123", fake_options(d));
  discord::Activity a;
  a.details = "first";
  p.set(a);
  REQUIRE(eventually([&] { return d.activities().size() == 1; }));
  const auto messages = d.messages();
  CHECK(messages[0].op == Opcode::Handshake);
  CHECK(contains(messages[0].json, R"("client_id":"123")"));
  CHECK(contains(d.activities()[0], R"("details":"first")"));
  CHECK(p.connected());

  // Quick changes: only the last is sent, once the interval has passed.
  const auto sent = std::chrono::steady_clock::now();
  a.details = "second";
  p.set(a);
  a.details = "third";
  p.set(a);
  REQUIRE(eventually([&] { return d.activities().size() == 2; }));
  CHECK(std::chrono::steady_clock::now() - sent >= std::chrono::milliseconds(250));
  CHECK(contains(d.activities()[1], R"("details":"third")"));
  // The same activity again sends nothing.
  p.set(a);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(d.activities().size() == 2);
}

TEST_CASE("discord: the client reconnects and shows the activity again") {
  FakeDiscord d;
  discord::Presence p("123", fake_options(d));
  discord::Activity a;
  a.details = "racing";
  p.set(a);
  REQUIRE(eventually([&] { return d.activities().size() == 1; }));
  {
    std::lock_guard lock(d.mutex);
    d.hang_up = true;  // Discord quits
  }
  REQUIRE(eventually([&] { return d.activities().size() == 2; }));
  CHECK(d.connections == 2);
  CHECK(contains(d.activities()[1], R"("details":"racing")"));
}

TEST_CASE("discord: an application Discord turns down is tried again later") {
  FakeDiscord d;
  d.accept = false;
  discord::Presence p("bad", fake_options(d));
  discord::Activity a;
  a.details = "x";
  p.set(a);
  REQUIRE(eventually([&] { return d.connections >= 2; }));
  CHECK(d.activities().empty());
  CHECK_FALSE(p.connected());
}

TEST_CASE("rich presence: what the games' status shows") {
  using Scene = app::GameStatus::Scene;
  CHECK(app::ordinal(1) == "1st");
  CHECK(app::ordinal(2) == "2nd");
  CHECK(app::ordinal(3) == "3rd");
  CHECK(app::ordinal(4) == "4th");
  CHECK(app::ordinal(11) == "11th");
  CHECK(app::ordinal(22) == "22nd");

  app::GameStatus s;
  s.game_image = "gp2";
  s.scene = Scene::Attract;
  s.character = "Mario";
  s.character_image = "mario";
  discord::Activity a = app::describe(s, "Mario Kart Arcade GP 2", 100, 0);
  CHECK(a.details == "Mario Kart Arcade GP 2");
  CHECK(a.state == "In the attract mode");
  CHECK(a.start == 100);
  CHECK(a.large_image == "gp2");
  CHECK(a.small_image.empty());  // the attract mode's demo racers are nobody's

  s.scene = Scene::Menus;
  s.activity = "Choosing a course";
  a = app::describe(s, "Mario Kart Arcade GP 2", 100, 0);
  CHECK(a.state == "Choosing a course");
  CHECK(a.small_image == "mario");
  CHECK(a.small_text == "Mario");

  s.scene = Scene::Racing;
  s.mode = "Grand Prix";
  s.engine = "100cc";
  s.cup = "Yoshi Cup";
  s.cup_image = "cup_yoshi";
  s.course = "Yoshi Park 1";
  s.round = 1;
  a = app::describe(s, "Mario Kart Arcade GP 2", 100, 0);
  CHECK(a.details == "Yoshi Cup · Round 1: Yoshi Park 1");
  CHECK(a.state == "Mario");
  CHECK(a.start == 100);
  CHECK(a.large_image == "cup_yoshi");
  CHECK(a.large_text == "Yoshi Cup, 100cc");

  s.started = true;
  s.position = 3;
  s.lap = 2;
  s.laps = 3;
  a = app::describe(s, "Mario Kart Arcade GP 2", 100, 160);
  CHECK(a.state == "Mario · 3rd · Lap 2/3");
  CHECK(a.start == 160);  // the race's time

  s.lap = 4;  // past the finish line
  CHECK(app::describe(s, "", 100, 160).state == "Mario · 3rd · Lap 3/3");

  s.scene = Scene::Finished;
  a = app::describe(s, "Mario Kart Arcade GP 2", 100, 0);
  CHECK(a.details == "Yoshi Cup · Round 1: Yoshi Park 1");
  CHECK(a.state == "Race over");

  app::GameStatus tt;
  tt.scene = Scene::Racing;
  tt.mode = "Time Trial";
  tt.course = "Diamond City (reverse)";
  tt.character = "Luigi";
  tt.started = true;
  tt.lap = 1;
  tt.laps = 3;
  a = app::describe(tt, "Mario Kart Arcade GP 2", 100, 200);
  CHECK(a.details == "Time Trial · Diamond City (reverse)");
  CHECK(a.state == "Luigi · Lap 1/3");
}
