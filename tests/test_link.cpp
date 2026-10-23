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
#include <process.h>
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

void close_socket(decltype(::socket(0, 0, 0)) s) {
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

// The tests' first host port: a base from 47600 to 49079 whose 40 ports
// (four cabinets, ten each) are free on this computer, so another program
// holding 47600 doesn't fail the tests. The search starts at a place picked
// by the process ID, so two test runs started together (two builds' ctest)
// choose different bases.
u16 pick_port() {
#ifdef _WIN32
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
  const u32 pid = static_cast<u32>(_getpid());
#else
  const u32 pid = static_cast<u32>(::getpid());
#endif
  constexpr u32 kBases = 37;  // 47600 + 37 * 40 = 49080, below the ephemeral ports
  for (u32 i = 0; i < kBases; ++i) {
    const u32 base = 47600 + (pid + i) % kBases * 40;
    bool free = true;
    for (u32 port = base; port < base + 40 && free; ++port) {
      for (const int type : {SOCK_STREAM, SOCK_DGRAM}) {
        const auto s = ::socket(AF_INET, type, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u16>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        free = free && ::bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0;
        close_socket(s);
      }
    }
    if (free) return static_cast<u16>(base);
  }
  return 47600;
}

const u16 kPort = pick_port();

// A cabinet: media board memory at 0x1000 and its network, linked.
struct Cabinet {
  std::vector<u8> memory = std::vector<u8>(0x10000);
  triforce::Network net{[this](u32 a) {
    return a >= 0x1000 && a < 0x1000 + memory.size() ? std::span<u8>(memory).subspan(a - 0x1000) : std::span<u8>();
  }};

  Cabinet(int number, const char* game = "GGPE02") {
    net.enable_link({game, kPort, false, "test"});
    CHECK(net.link_status().error == "");
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

  // Both ends send each message at once: with Nagle's algorithm, a cabinet's
  // messages to another computer would wait for acknowledgements and come
  // in bursts (loopback hides it, so check the setting itself).
  CHECK(one.net.sends_without_delay(client));
  CHECK(two.net.sends_without_delay(accepted));
  CHECK_FALSE(two.net.sends_without_delay(listener));

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

namespace {

// A beacon as OpenGP sends it (see network.cpp), from a socket of the test's
// own, to cabinet `to`'s beacon port on this computer.
void send_beacon(int to, u8 version, int cabinet, u32 nonce, bool leaving, const char* game = "GGPE02") {
  u8 b[40] = {};
  std::memcpy(b, "OGPB", 4);
  b[4] = version;
  b[5] = static_cast<u8>(cabinet);
  b[8] = static_cast<u8>(nonce >> 24), b[9] = static_cast<u8>(nonce >> 16);
  b[10] = static_cast<u8>(nonce >> 8), b[11] = static_cast<u8>(nonce);
  std::memcpy(b + 12, game, std::strlen(game));
  b[20] = leaving ? 1 : 0;
  std::memcpy(b + 24, "9.9.9", 5);
  const auto s = ::socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<u16>(kPort + (to - 1) * 10 + 9));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::sendto(s, reinterpret_cast<const char*>(b), version >= 2 ? 40 : 24, 0, reinterpret_cast<const sockaddr*>(&a),
           sizeof(a));
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

}  // namespace

TEST_CASE("another version of OpenGP is named, not linked") {
  Cabinet one(1);
  REQUIRE(run_until({&one}, [&] { return one.net.link_status().cabinet == 1; }));
  bool named = false;
  run_until({&one}, [&] {
    send_beacon(1, 1, 2, 0x1234, false);
    for (const std::string& s : one.net.link_status().incompatible) named |= s.find("older OpenGP") != std::string::npos;
    return named;
  });
  CHECK(named);
  CHECK_FALSE(one.net.link_status().found[2]);
  named = false;
  run_until({&one}, [&] {
    send_beacon(1, 9, 3, 0x5678, false);
    for (const std::string& s : one.net.link_status().incompatible) named |= s.find("OpenGP 9.9.9") != std::string::npos;
    return named;
  });
  CHECK(named);
}

TEST_CASE("a cabinet's number stays with the cabinet that has it") {
  Cabinet one(1);
  REQUIRE(run_until({&one}, [&] { return one.net.link_status().cabinet == 1; }));
  // A cabinet 3 is found...
  REQUIRE(run_until({&one}, [&] {
    send_beacon(1, 2, 3, 0xAAAA, false);
    return one.net.link_status().found[3];
  }));
  // ... and another sender saying it is 3 cannot send it away.
  send_beacon(1, 2, 3, 0xBBBB, true);
  run_until({&one}, [] { return false; }, 300);
  CHECK(one.net.link_status().found[3]);
  // Its own goodbye does.
  send_beacon(1, 2, 3, 0xAAAA, true);
  CHECK(run_until({&one}, [&] { return !one.net.link_status().found[3]; }, 1000));
}

TEST_CASE("a hello for another game is not taken") {
  Cabinet two(2);
  const u32 listener = two.listen(5000);
  const auto s = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kPort + 10);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
  u8 hello[16] = {'O', 'G', 'P', 'L', 2, 1};
  std::memcpy(hello + 8, "GGPE01", 6);
  ::send(s, reinterpret_cast<const char*>(hello), 16, 0);
  CHECK_FALSE(run_until({&two}, [&] { return two.readable(listener); }, 1000));
  CHECK(two.run(triforce::NET_ACCEPT, {listener, 0x1400, 0x1410}) == kError);
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

namespace {

u32 ipv4(const char* text) {
  u32 ip = 0;
  CHECK(triforce::parse_ipv4(text, &ip));
  return ip;
}

}  // namespace

TEST_CASE("the link takes beacons and connections from local networks only") {
  using triforce::link_allows;
  const std::vector<triforce::Subnet> none;
  // This computer, always; nothing else without linking on the local network.
  CHECK(link_allows(ipv4("127.0.0.1"), false, none));
  CHECK_FALSE(link_allows(ipv4("192.168.1.20"), false, none));
  // With it, private, link-local and shared (CGNAT) addresses, but not the
  // internet.
  for (const char* a : {"127.0.0.1", "10.1.2.3", "172.16.0.5", "172.31.255.1", "192.168.1.20", "169.254.3.4",
                        "100.64.0.1", "100.127.255.254"})
    CHECK_MESSAGE(link_allows(ipv4(a), true, none), a);
  for (const char* a : {"8.8.8.8", "172.32.0.1", "100.128.0.1", "26.1.2.3", "25.1.2.3", "203.0.113.200"})
    CHECK_MESSAGE(!link_allows(ipv4(a), true, none), a);

  // A network this computer is on counts too: a virtual LAN on public
  // addresses (Radmin VPN's 26/8, Hamachi's 25/8), or a LAN on a public range.
  const std::vector<triforce::Subnet> own = {
      {ipv4("26.10.20.30"), 0xFF000000}, {ipv4("25.1.1.1"), 0xFF000000}, {ipv4("203.0.113.7"), 0xFFFFFF00}};
  for (const char* a : {"26.99.1.2", "25.200.3.4", "203.0.113.200", "192.168.1.20"})
    CHECK_MESSAGE(link_allows(ipv4(a), true, own), a);
  for (const char* a : {"8.8.8.8", "27.1.2.3", "203.0.114.1"}) CHECK_MESSAGE(!link_allows(ipv4(a), true, own), a);
  CHECK_FALSE(link_allows(ipv4("26.99.1.2"), false, own));

  // A network wider than a /8 (an odd netmask) never lets the internet in.
  const std::vector<triforce::Subnet> wide = {{ipv4("26.10.20.30"), 0}, {ipv4("26.10.20.30"), 0xFE000000}};
  for (const char* a : {"8.8.8.8", "27.1.2.3"}) CHECK_MESSAGE(!link_allows(ipv4(a), true, wide), a);
}
