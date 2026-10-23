// SPDX-License-Identifier: GPL-3.0-or-later
//
// The media board's network as the games use it: a socket API (the board
// commands 0x401-0x415) whose sockets are numbered 1-63. Each socket is a
// host socket, or a connection to a service that OpenGP plays itself, such as
// the NamCam2 camera (camera/namcam.h), which needs no host network at all.
//
// Where a game's connections go:
// - to a service registered for their address and port;
// - to a host address routed for it (add_route);
// - to another cabinet, once linking is on (enable_link, below);
// - nowhere else: other connections are refused. Sockets bind to the
//   loopback interface, unless linking on the local network is on.
//
// Host sockets are non-blocking: the game polls them (select with a zero
// timeout, once a frame), and a board command must not stop the game.
// Connecting waits for the host's answer, up to the game's timeout.
//
// Based on the socket commands of Dolphin's DVD/AMMediaboard.cpp (Copyright
// Dolphin Emulator Project, GPL-2.0-or-later).
#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gekko/types.h"

namespace triforce {

using namespace gekko;

// One connection to a service. What the game sends arrives in on_receive();
// the service answers with reply() and may then close() its end, after which
// the game reads the rest of the reply and then the end of the stream.
class ServiceConnection {
 public:
  virtual ~ServiceConnection() = default;
  virtual void on_receive(std::span<const u8> data) = 0;

 protected:
  void reply(std::span<const u8> data) { out_.insert(out_.end(), data.begin(), data.end()); }
  void close() { closed_ = true; }

 private:
  friend class Network;
  std::vector<u8> out_;
  std::size_t out_pos_ = 0;
  bool closed_ = false;
};

// Something OpenGP answers on the media board's network.
class Service {
 public:
  virtual ~Service() = default;
  virtual std::unique_ptr<ServiceConnection> accept() = 0;
};

// Board commands of the socket API.
enum : u16 {
  NET_ACCEPT = 0x401,
  NET_BIND = 0x402,
  NET_CLOSESOCKET = 0x403,
  NET_CONNECT = 0x404,
  NET_INET_ADDR = 0x406,
  NET_LISTEN = 0x408,
  NET_RECV = 0x409,
  NET_SEND = 0x40A,
  NET_SOCKET = 0x40B,
  NET_SELECT = 0x40C,
  NET_SET_SOCK_OPT = 0x40E,
  NET_SET_TIMEOUTS = 0x410,
  NET_GET_LAST_ERROR = 0x411,
  NET_GET_PARAM_BY_DHCP_EXEC = 0x414,
  NET_MODIFY_MY_IP_ADDR = 0x415,
};

// Socket status codes (GetLastError). Mario Kart GP 2 describes them all (in
// Japanese); they are close to BSD's errno values.
enum : u32 {
  SSC_EBADF = 9,
  SSC_EFAULT = 14,
  SSC_EMFILE = 24,
  SSC_EADDRINUSE = 48,
  SSC_ENOTCONN = 57,
  SSC_ETIMEDOUT = 60,
  SSC_ECONNREFUSED = 61,
  SSC_EHOSTUNREACH = 65,
  SSC_EWOULDBLOCK = 70,  // also success: the board reports both as 70
  SSC_SUCCESS = 70,
};

// An IPv4 address and port, both in host byte order (192.168.29.104 is
// 0xC0A81D68).
struct Endpoint {
  u32 ip = 0;
  u16 port = 0;
  auto operator<=>(const Endpoint&) const = default;
};

// Linked cabinets (milestone 12). Up to four cabinets link, numbered 1-4 (the
// games' PCB ID): cabinet n gives itself 192.168.29.150 + n - 1 and listens on
// one of ports 5000-5008, and the others connect to it there. Each OpenGP
// plays one cabinet. On the host, cabinet n's port 5000 + k is
// `port` + 10 (n - 1) + k, so four can run on one computer, and its port
// `port` + 10 (n - 1) + 9 takes the beacons with which the cabinets find each
// other: twice a second, each tells the others its number, its game and the
// ports it listens on, and a cabinet connects to another only on a port it
// has said it listens on (so a connection is never left waiting for a cabinet
// that is not there). Each connection starts with a hello that names the
// cabinet and its game, so the one accepting it knows who it is, and only the
// same game links.
constexpr u16 kLinkPort = 28500;

struct LinkOptions {
  std::string game;      // the game's ID (GGPE02): only cabinets playing the same game link
  u16 port = kLinkPort;  // the first host port (above)
  bool lan = false;      // also link over the local network: listen on every interface, broadcast the beacons
  std::string version;   // OpenGP's version, which the beacons carry (cabinets of other versions do not link)
};

// What the link sees, for the menu.
struct LinkStatus {
  bool on = false;                  // linking is enabled
  int cabinet = 0;                  // this cabinet's number, 0 until the game has given itself one
  std::array<bool, 5> found{};      // by number: cabinets heard from in the last few seconds
  std::array<bool, 5> connected{};  // by number: cabinets with a connection open
  std::array<bool, 5> linked{};     // by number: ... both ways (the games make one each way)
  bool clash = false;               // another cabinet has this one's number
  std::string error;                // why the link cannot run (a port in use), or ""
  // OpenGP at another address that cannot link with this one (another
  // version), heard from in the last few seconds: "192.168.1.20 (0.4.0)".
  std::vector<std::string> incompatible;
};

// "a.b.c.d" in host byte order; false if `text` is not one.
bool parse_ipv4(std::string_view text, u32* ip);
std::string ipv4_string(u32 ip);

class Network {
 public:
  // `memory` returns the media board memory at an address, to the end of its
  // buffer (empty if there is none): the game's data for send and recv, and
  // the addresses, strings and fd sets of other commands.
  explicit Network(std::function<std::span<u8>(u32 address)> memory);
  ~Network();
  Network(const Network&) = delete;
  Network& operator=(const Network&) = delete;

  // Connections to `address` go to `service`.
  void add_service(Endpoint address, std::shared_ptr<Service> service);
  // Connections to `address` go to the host's `to` instead.
  void add_route(Endpoint address, Endpoint to);

  // Links this cabinet with the others (see LinkOptions).
  void enable_link(const LinkOptions& options);
  LinkStatus link_status() const;  // thread-safe
  // Tells the other cabinets that this one is stopping. For a process that
  // ends without running destructors; thread-safe.
  void stop_link();

  // Runs socket API command `command` (NET_*) with its parameters (the
  // command block's words after the command) and returns its result.
  u32 execute(u16 command, std::span<const u32> params);

  // The address the game gave itself (ModifyMyIPaddr), 0 before it has.
  u32 own_address() const { return own_address_; }

  // Whether the game has linked to another cabinet since start-up: a routed
  // connection made, or one accepted on a listening socket (services such as
  // the camera do not count). The other cabinets expect this one to keep
  // running, so the launcher does not pause a linked game. Thread-safe.
  bool linked() const { return linked_.load(std::memory_order_relaxed); }

  // Whether socket `fd`'s connection on the host sends each write at once
  // (TCP_NODELAY, which every link connection gets). For tests.
  bool sends_without_delay(u32 fd) const;

  static constexpr std::size_t kSockets = 64;  // numbers 1-63; 0 is never used

 private:
  struct Socket;
  class Link;

  u32 socket(u32 domain, u32 type);
  u32 closesocket(u32 fd);
  u32 connect(u32 fd, u32 address, u32 length);
  u32 bind(u32 fd, u32 address, u32 length);
  u32 listen(u32 fd, u32 backlog);
  u32 accept(u32 fd, u32 address, u32 length_address);
  u32 send(u32 fd, u32 data, u32 length);
  u32 recv(u32 fd, u32 data, u32 length);
  u32 select(u32 count, u32 read_set, u32 write_set, u32 except_set, u32 timeout);
  u32 setsockopt(u32 fd, u32 level, u32 name, u32 value, u32 length);
  u32 set_timeouts(u32 fd, u32 connect_ms, u32 send_ms, u32 recv_ms);
  u32 inet_addr(u32 text);
  void modify_my_ip_addr(u32 text);

  u32 host_connect(Socket& s, u32 fd, Endpoint to, Endpoint host, u32 max_wait_ms = 0xFFFFFFFF);
  u32 flush(Socket& s);
  int own_cabinet() const;
  void update_link();
  void take_incoming(Socket& listener);

  Socket* find(u32 fd);
  u32 new_socket();
  std::string_view string_at(u32 address, std::size_t max_length);
  u32 fail(u32 error);

  std::function<std::span<u8>(u32)> memory_;
  std::array<std::unique_ptr<Socket>, kSockets> sockets_;
  u32 next_fd_ = 1;
  u32 last_error_ = SSC_SUCCESS;
  u32 own_address_ = 0;
  std::atomic<bool> linked_{false};
  std::map<Endpoint, std::shared_ptr<Service>> services_;
  std::map<Endpoint, Endpoint> routes_;
  std::unique_ptr<Link> link_;
};

}  // namespace triforce
