// SPDX-License-Identifier: GPL-3.0-or-later
//
// Linked cabinets (triforce/network.h, LinkOptions): cabinets on one computer
// find each other, connect on the ports they listen on, and know who
// connected.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "triforce/network.h"

using namespace gekko;

namespace {

constexpr u32 kError = 0xFFFFFFFF;
constexpr u16 kPort = 47600;  // the tests' first host port

// A cabinet: media board memory at 0x1000 and its network, linked.
struct Cabinet {
  std::vector<u8> memory = std::vector<u8>(0x10000);
  triforce::Network net{[this](u32 a) {
    return a >= 0x1000 && a < 0x1000 + memory.size() ? std::span<u8>(memory).subspan(a - 0x1000) : std::span<u8>();
  }};

  Cabinet(int number, const char* game = "GGPE02") {
    net.enable_link({game, kPort, false});
    const std::string address = "192.168.29." + std::to_string(149 + number);
    std::memcpy(at(0x1F00), address.c_str(), address.size() + 1);
    run(triforce::NET_MODIFY_MY_IP_ADDR, {0x1F00});
  }
  u8* at(u32 a) { return &memory[a - 0x1000]; }
  u32 run(u16 command, std::initializer_list<u32> params) {
    const std::vector<u32> p(params);
    return net.execute(command, p);
  }
  void put_address(u32 a, u32 ip, u16 port) {
    u8* p = at(a);
    std::memset(p, 0, 16);
    p[0] = 16, p[1] = 2, p[2] = static_cast<u8>(port >> 8), p[3] = static_cast<u8>(port);
    p[4] = static_cast<u8>(ip >> 24), p[5] = static_cast<u8>(ip >> 16), p[6] = static_cast<u8>(ip >> 8), p[7] = static_cast<u8>(ip);
  }
  // A socket listening on the game's port `port`.
  u32 listen(u16 port) {
    const u32 fd = run(triforce::NET_SOCKET, {2, 1, 0});
    put_address(0x1000, 0, port);
    REQUIRE(run(triforce::NET_BIND, {fd, 0x1000, 16}) == 0);
    REQUIRE(run(triforce::NET_LISTEN, {fd, 5}) == 0);
    return fd;
  }
  // Connects to cabinet `number`'s port `port`: the socket, or kError.
  u32 connect(int number, u16 port) {
    const u32 fd = run(triforce::NET_SOCKET, {2, 1, 0});
    run(triforce::NET_SET_TIMEOUTS, {fd, 2000, 2000, 2000});
    put_address(0x1000, 0xC0A81D96 + static_cast<u32>(number - 1), port);
    if (run(triforce::NET_CONNECT, {fd, 0x1000, 16}) == 0) return fd;
    run(triforce::NET_CLOSESOCKET, {fd});
    return kError;
  }
  // Whether `fd` is readable (select with no wait).
  bool readable(u32 fd) {
    std::memset(at(0x1100), 0, 32);
    at(0x1100)[fd / 8] = static_cast<u8>(1 << (fd % 8));
    std::memset(at(0x1200), 0, 8);
    return run(triforce::NET_SELECT, {64, 0x1100, 0, 0, 0x1200}) == 1;
  }
};

// Runs the cabinets' networks (as the games do, each frame) until `done`.
bool run_until(std::initializer_list<Cabinet*> cabinets, const std::function<bool()>& done, int ms = 3000) {
  for (int i = 0; i < ms / 10; ++i) {
    for (Cabinet* c : cabinets) c->run(triforce::NET_GET_LAST_ERROR, {});
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

TEST_CASE("two cabinets on one computer link") {
  Cabinet one(1), two(2);
  CHECK(one.connect(2, 5000) == kError);  // two is not listening yet
  const u32 listener = two.listen(5000);
  REQUIRE(run_until({&one, &two}, [&] { return one.net.link_status().found[2] && two.net.link_status().found[1]; }));
  CHECK(one.net.link_status().cabinet == 1);
  CHECK(two.net.link_status().cabinet == 2);
  CHECK(one.connect(2, 5001) == kError);  // not a port two listens on
  CHECK(one.connect(3, 5000) == kError);  // no cabinet 3
  CHECK_FALSE(one.net.linked());

  const u32 client = one.connect(2, 5000);
  REQUIRE(client != kError);
  CHECK(one.net.linked());

  // Two accepts it once the hello has come, as a connection from cabinet 1.
  u32 accepted = kError;
  REQUIRE(run_until({&one, &two}, [&] {
    if (!two.readable(listener)) return false;
    std::memset(two.at(0x1410), 0, 4);
    accepted = two.run(triforce::NET_ACCEPT, {listener, 0x1400, 0x1410});
    return accepted != kError;
  }));
  CHECK(two.net.linked());
  CHECK(two.at(0x1404)[0] == 192);
  CHECK(two.at(0x1404)[1] == 168);
  CHECK(two.at(0x1404)[2] == 29);
  CHECK(two.at(0x1404)[3] == 150);

  // The game's data, without the hello, both ways.
  std::memcpy(one.at(0x2000), "lap 3", 5);
  CHECK(one.run(triforce::NET_SEND, {client, 0x2000, 5}) == 5);
  u32 n = kError;
  REQUIRE(run_until({&two}, [&] { return two.readable(accepted) && (n = two.run(triforce::NET_RECV, {accepted, 0x3000, 64})) != kError; }));
  CHECK(n == 5);
  CHECK(std::memcmp(two.at(0x3000), "lap 3", 5) == 0);
  std::memcpy(two.at(0x2000), "1st", 3);
  CHECK(two.run(triforce::NET_SEND, {accepted, 0x2000, 3}) == 3);
  REQUIRE(run_until({&one}, [&] { return one.readable(client) && (n = one.run(triforce::NET_RECV, {client, 0x3000, 64})) != kError; }));
  CHECK(n == 3);
  CHECK(std::memcmp(one.at(0x3000), "1st", 3) == 0);

  REQUIRE(run_until({&one, &two}, [&] { return one.net.link_status().connected[2] && two.net.link_status().connected[1]; }));
  CHECK(one.run(triforce::NET_CLOSESOCKET, {client}) == 0);
  REQUIRE(run_until({&one}, [&] { return !one.net.link_status().connected[2]; }));
}

TEST_CASE("only cabinets playing the same game link") {
  Cabinet one(1), other(3, "GGPE01");
  other.listen(5000);
  run_until({&one, &other}, [] { return false; }, 1200);  // a few beacons go round
  CHECK_FALSE(one.net.link_status().found[3]);
  CHECK(one.connect(3, 5000) == kError);
}

TEST_CASE("a connection without a hello is not accepted") {
  Cabinet two(2);
  const u32 listener = two.listen(5000);
  // Something else connects to two's host port for 5000, and says nothing.
  const auto s = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kPort + 10);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
  ::send(s, "GET / HTTP/1.0\r\n\r\n", 18, 0);
  CHECK_FALSE(run_until({&two}, [&] { return two.readable(listener); }, 1000));
  CHECK(two.run(triforce::NET_ACCEPT, {listener, 0x1400, 0x1410}) == kError);
  CHECK_FALSE(two.net.linked());
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

TEST_CASE("two cabinets with the same number on one computer") {
  Cabinet first(1), other(2);
  // Once another cabinet hears the first, its beacons are running.
  REQUIRE(run_until({&first, &other}, [&] { return other.net.link_status().found[1]; }));
  CHECK(first.net.link_status().error.empty());
  Cabinet second(1);
  REQUIRE(run_until({&second}, [&] { return !second.net.link_status().error.empty(); }));
  CHECK(second.net.link_status().error.find("cabinet 1") != std::string::npos);
}

TEST_CASE("cabinets find each other while the game is busy, and see one stop") {
  Cabinet one(1);
  auto wait = [](const std::function<bool()>& done, int ms) {
    for (int i = 0; i < ms / 10 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return done();
  };
  {
    Cabinet two(2);
    // No board commands: the beacons go round by themselves.
    REQUIRE(wait([&] { return one.net.link_status().found[2] && two.net.link_status().found[1]; }, 3000));
  }
  // Two says goodbye as it stops, so one knows at once (well before the
  // beacons' timeout).
  CHECK(wait([&] { return !one.net.link_status().found[2]; }, 1000));
}
