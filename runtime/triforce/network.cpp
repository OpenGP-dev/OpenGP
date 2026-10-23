// SPDX-License-Identifier: GPL-3.0-or-later
//
// Based on the socket commands of Dolphin's DVD/AMMediaboard.cpp (Copyright
// Dolphin Emulator Project, GPL-2.0-or-later).

#include "triforce/network.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace triforce {

namespace {

// ---- Host sockets --------------------------------------------------------------------------------

#ifdef _WIN32
using HostSocket = SOCKET;
constexpr HostSocket kNoSocket = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
int close_host(HostSocket s) { return ::closesocket(s); }
int host_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
int host_poll(PollFd* fds, std::size_t count, int timeout_ms) {
  return WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
}
void set_nonblocking(HostSocket s) {
  u_long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}
void set_no_inherit(HostSocket s) { SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0); }
constexpr int kSendFlags = 0;
constexpr short kPollPri = 0;  // Winsock refuses POLLPRI (WSAEINVAL)
#else
using HostSocket = int;
constexpr HostSocket kNoSocket = -1;
using PollFd = pollfd;
int close_host(HostSocket s) { return ::close(s); }
int host_error() { return errno; }
bool would_block(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS; }
int host_poll(PollFd* fds, std::size_t count, int timeout_ms) {
  int r;
  do r = ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
  while (r < 0 && errno == EINTR);
  return r;
}
void set_nonblocking(HostSocket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
void set_no_inherit(HostSocket s) { fcntl(s, F_SETFD, FD_CLOEXEC); }
constexpr short kPollPri = POLLPRI;
#if defined(__linux__)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

// A new host socket for the guest: non-blocking, never raising SIGPIPE, and
// not inherited by the process a reboot starts (it would keep the link's
// ports bound and the other cabinets' connections open).
HostSocket open_host(int type) {
  const HostSocket s = ::socket(AF_INET, type, 0);
  if (s == kNoSocket) return s;
  set_nonblocking(s);
  set_no_inherit(s);
#if defined(__APPLE__)
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  return s;
}

// Sends each of the game's writes at once (no Nagle's algorithm). A link
// connection carries messages one way only, one a frame, so with Nagle a
// message waits for the acknowledgement of the one before, which the other
// computer may delay (up to 200 ms on Windows): over a network, a cabinet's
// messages then come in bursts. The games never ask for TCP_NODELAY.
void set_no_delay(HostSocket s) {
  const int on = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

// The host's errors as the board reports them.
u32 status_for(int error) {
  if (would_block(error)) return SSC_EWOULDBLOCK;
#ifdef _WIN32
  switch (error) {
    case WSAECONNREFUSED: return SSC_ECONNREFUSED;
    case WSAETIMEDOUT: return SSC_ETIMEDOUT;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH: return SSC_EHOSTUNREACH;
    case WSAEADDRINUSE: return SSC_EADDRINUSE;
    case WSAENOTCONN: return SSC_ENOTCONN;
    default: return SSC_EBADF;
  }
#else
  switch (error) {
    case ECONNREFUSED: return SSC_ECONNREFUSED;
    case ETIMEDOUT: return SSC_ETIMEDOUT;
    case EHOSTUNREACH:
    case ENETUNREACH: return SSC_EHOSTUNREACH;
    case EADDRINUSE: return SSC_EADDRINUSE;
    case ENOTCONN: return SSC_ENOTCONN;
    default: return SSC_EBADF;
  }
#endif
}

sockaddr_in host_address(Endpoint e) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(e.port);
  a.sin_addr.s_addr = htonl(e.ip);
  return a;
}

// The broadcast addresses of the computer's networks (host byte order), so
// that the link's beacons reach the local network whichever one it is (a
// broadcast to 255.255.255.255 leaves by one network only, which may be a
// VPN's or a virtual machine's).
std::vector<u32> broadcast_addresses() {
  std::vector<u32> out;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  std::vector<u8> buffer(size);
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
  if (r == ERROR_BUFFER_OVERFLOW) {
    buffer.resize(size);
    first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    r = GetAdaptersAddresses(AF_INET, flags, nullptr, first, &size);
  }
  if (r != NO_ERROR) return out;
  for (const IP_ADAPTER_ADDRESSES* a = first; a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET || u->OnLinkPrefixLength == 0 || u->OnLinkPrefixLength > 30)
        continue;
      const u32 ip = ntohl(reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
      out.push_back(ip | (0xFFFFFFFFu >> u->OnLinkPrefixLength));
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (const ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_broadaddr) continue;
    if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    out.push_back(ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr));
  }
  freeifaddrs(list);
#endif
  out.push_back(INADDR_BROADCAST);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// ---- The guest's structures ------------------------------------------------------------------------

// sockaddr_in as the board's VxWorks has it: length, family, then the port
// and address in network byte order, and 8 bytes of padding.
constexpr u32 kGuestAddressSize = 16;
constexpr u8 kGuestFamilyInet = 2;

Endpoint read_address(const u8* p) {
  return {(u32{p[4]} << 24) | (u32{p[5]} << 16) | (u32{p[6]} << 8) | p[7], static_cast<u16>((p[2] << 8) | p[3])};
}

void write_address(u8* p, Endpoint e) {
  std::memset(p, 0, kGuestAddressSize);
  p[0] = kGuestAddressSize;
  p[1] = kGuestFamilyInet;
  p[2] = static_cast<u8>(e.port >> 8), p[3] = static_cast<u8>(e.port);
  p[4] = static_cast<u8>(e.ip >> 24), p[5] = static_cast<u8>(e.ip >> 16);
  p[6] = static_cast<u8>(e.ip >> 8), p[7] = static_cast<u8>(e.ip);
}

// fd_set: 256 bits, socket n at bit n % 8 of byte n / 8 (little-endian words).
constexpr u32 kFdSetSize = 32;

u32 le32(const u8* p) { return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24); }
void put_le32(u8* p, u32 v) {
  p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8), p[2] = static_cast<u8>(v >> 16), p[3] = static_cast<u8>(v >> 24);
}

// Socket types and options: BSD's values.
constexpr u32 kGuestStream = 1, kGuestDatagram = 2;
constexpr u32 kGuestSolSocket = 0xFFFF, kGuestIpprotoTcp = 6;
constexpr u32 kGuestReuseAddr = 0x0004, kGuestKeepAlive = 0x0008, kGuestBroadcast = 0x0020;
constexpr u32 kGuestTcpNoDelay = 0x0001;

// Never block the game for longer than this in one command.
constexpr int kMaxWaitMs = 2000;

bool log_enabled() {
  static const bool enabled = std::getenv("OPENGP_LOG_NETWORK") != nullptr;
  return enabled;
}

constexpr u32 kError = 0xFFFFFFFF;

// ---- Linked cabinets (see LinkOptions) -------------------------------------------------------------

using Clock = std::chrono::steady_clock;

constexpr u32 kFirstCabinet = 0xC0A81D96;  // 192.168.29.150, cabinet 1
constexpr int kCabinets = 4;
constexpr u16 kGamePort = 5000;
constexpr u16 kGamePorts = 9;  // 5000-5008: a cabinet moves on to the next when it listens again
constexpr int kPortsPerCabinet = 10;
// 2: OpenGP 1.0 (TCP_NODELAY; the beacon carries OpenGP's version). The first
// six bytes of the hello and the beacon (magic, version, cabinet) stay the
// same in every version, so that any OpenGP can tell another version's.
constexpr u8 kLinkVersion = 2;
constexpr std::size_t kHelloSize = 16, kBeaconSize = 40, kBeaconVersionAt = 24, kBeaconVersionSize = 16;
constexpr std::size_t kMaxIncoming = 8;               // connections waiting on a link listener
constexpr auto kAcceptTimeout = std::chrono::seconds(30);  // ... for the game to take them
constexpr std::size_t kMaxQueued = 1 << 20;           // bytes a link connection holds for a slow reader
constexpr u32 kLinkConnectWaitMs = 500;               // a cabinet on the network answers in a few ms
constexpr auto kBeaconInterval = std::chrono::milliseconds(500);
constexpr auto kPeerTimeout = std::chrono::seconds(5);  // a cabinet not heard from for this long is gone
constexpr auto kHelloTimeout = std::chrono::seconds(5);
constexpr auto kClashTimeout = std::chrono::seconds(3);

// The cabinet at a game address (1-4), or 0.
int cabinet_at(u32 ip) {
  return ip >= kFirstCabinet && ip < kFirstCabinet + kCabinets ? static_cast<int>(ip - kFirstCabinet) + 1 : 0;
}
bool is_game_port(u16 port) { return port >= kGamePort && port < kGamePort + kGamePorts; }
u16 host_port(u16 first, int cabinet, u16 game_port) {
  return static_cast<u16>(first + (cabinet - 1) * kPortsPerCabinet + (game_port - kGamePort));
}
u16 beacon_port(u16 first, int cabinet) { return static_cast<u16>(first + (cabinet - 1) * kPortsPerCabinet + 9); }

using GameTag = std::array<u8, 8>;  // the game's ID, padded with zeros

GameTag game_tag(const std::string& id) {
  GameTag tag{};
  std::memcpy(tag.data(), id.data(), std::min(id.size(), tag.size()));
  return tag;
}

// A connection's first bytes: "OGPL", the version, the cabinet, 2 bytes of 0,
// the game.
void write_hello(u8* p, int cabinet, const GameTag& game) {
  std::memset(p, 0, kHelloSize);
  std::memcpy(p, "OGPL", 4);
  p[4] = kLinkVersion;
  p[5] = static_cast<u8>(cabinet);
  std::memcpy(p + 8, game.data(), game.size());
}

// The cabinet a hello is from, or 0 if it is not one from the same game.
int read_hello(const u8* p, const GameTag& game) {
  if (std::memcmp(p, "OGPL", 4) != 0 || p[4] != kLinkVersion || std::memcmp(p + 8, game.data(), game.size()) != 0)
    return 0;
  return p[5] >= 1 && p[5] <= kCabinets ? p[5] : 0;
}

// A beacon: "OGPB", the version, the cabinet, the ports it listens on (bit k
// for 5000 + k, big endian), the sender's random number (to tell its own
// beacons and another cabinet with the same number apart), the game, flags
// (bit 0: leaving), 3 bytes of 0, and OpenGP's version (16 bytes, padded with
// zeros).
struct Beacon {
  int cabinet = 0;
  u16 listening = 0;
  u32 nonce = 0;
  bool leaving = false;  // the cabinet is stopping
};

// Addresses a beacon or connection may come from: this computer, and with
// linking on the local network, the private, link-local and shared (CGNAT,
// used by virtual LANs such as Tailscale) ranges. Not the internet.
bool is_loopback(u32 ip) { return (ip >> 24) == 127; }
bool is_local_network(u32 ip) {
  return is_loopback(ip) || (ip >> 24) == 10 || (ip >> 20) == (0xAC10u >> 4) || (ip >> 16) == 0xC0A8 ||
         (ip >> 16) == 0xA9FE || (ip >> 22) == (0x6440u >> 6);
}

// A beacon from OpenGP at all (any version, any game): its version byte.
bool beacon_magic(const u8* p, std::size_t size, u8* version) {
  if (size < 6 || std::memcmp(p, "OGPB", 4) != 0) return false;
  *version = p[4];
  return true;
}

void write_beacon(u8* p, const Beacon& b, const GameTag& game, const std::string& version) {
  std::memset(p, 0, kBeaconSize);
  std::memcpy(p + kBeaconVersionAt, version.data(), std::min(version.size(), kBeaconVersionSize));
  std::memcpy(p, "OGPB", 4);
  p[4] = kLinkVersion;
  p[5] = static_cast<u8>(b.cabinet);
  p[6] = static_cast<u8>(b.listening >> 8), p[7] = static_cast<u8>(b.listening);
  p[8] = static_cast<u8>(b.nonce >> 24), p[9] = static_cast<u8>(b.nonce >> 16);
  p[10] = static_cast<u8>(b.nonce >> 8), p[11] = static_cast<u8>(b.nonce);
  std::memcpy(p + 12, game.data(), game.size());
  p[20] = b.leaving ? 1 : 0;
}

bool read_beacon(const u8* p, std::size_t size, const GameTag& game, Beacon* b) {
  if (size < kBeaconSize || std::memcmp(p, "OGPB", 4) != 0 || p[4] != kLinkVersion ||
      std::memcmp(p + 12, game.data(), game.size()) != 0 || p[5] < 1 || p[5] > kCabinets)
    return false;
  b->cabinet = p[5];
  b->listening = static_cast<u16>((p[6] << 8) | p[7]);
  b->nonce = (u32{p[8]} << 24) | (u32{p[9]} << 16) | (u32{p[10]} << 8) | p[11];
  b->leaving = p[20] & 1;
  return true;
}

}  // namespace

// Seconds since the first message, to see how long linking takes.
double log_time() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

#define net_log(...)                                        \
  do {                                                      \
    if (log_enabled()) {                                    \
      std::fprintf(stderr, "[network] %8.3f ", log_time()); \
      std::fprintf(stderr, __VA_ARGS__);                    \
      std::fputc('\n', stderr);                             \
    }                                                       \
  } while (0)

bool parse_ipv4(std::string_view text, u32* ip) {
  u32 value = 0;
  int parts = 0;
  std::size_t i = 0;
  while (parts < 4) {
    u32 part = 0;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 3) part = part * 10 + (text[i++] - '0'), ++digits;
    if (digits == 0 || part > 255) return false;
    value = (value << 8) | part;
    if (++parts < 4) {
      if (i >= text.size() || text[i] != '.') return false;
      ++i;
    }
  }
  if (i != text.size()) return false;
  *ip = value;
  return true;
}

std::string ipv4_string(u32 ip) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
  return buf;
}

// A guest socket: a host socket until it connects to a service.
struct Network::Socket {
  // A connection from another cabinet whose hello has not all come yet.
  struct Incoming {
    HostSocket host = kNoSocket;
    Endpoint from;
    std::array<u8, kHelloSize> hello{};
    std::size_t received = 0;
    int cabinet = 0;    // who it says it is, once the hello has come
    bool ready = false;  // ... and it comes from that cabinet's address: the game can take it
    Clock::time_point since;
  };

  HostSocket host = kNoSocket;
  std::unique_ptr<ServiceConnection> service;
  u32 type = kGuestStream;
  u32 connect_timeout_ms = 2000;
  Endpoint peer;  // the address the game connected to

  // Linked cabinets.
  bool link_listener = false;  // bound to one of the game's link ports: connections start with a hello
  u16 game_port = 0;           // ... that one
  bool listening = false;
  int cabinet = 0;             // the cabinet at the other end of a connection
  bool accepted = false;       // ... which it made
  std::vector<Incoming> incoming;
  std::string queued;          // bytes the game sent that the host could not take yet (stream sockets)
  u64 sent = 0, received = 0;  // bytes, for the log

  ~Socket() {
    if (host != kNoSocket) close_host(host);
    for (const Incoming& in : incoming) close_host(in.host);
  }
  // Bytes the service has for the game.
  std::size_t pending() const { return service ? service->out_.size() - service->out_pos_ : 0; }
};

// Finding the other cabinets: the beacons. They run on a thread of their own
// from the moment the game has given itself its number, whatever the game is
// doing, so that the others are known before the game looks for them: it
// does that for a moment only, as it starts (the cabinet's 60-second LINK
// screen is skipped), and then only waits for them to connect.
class Network::Link {
 public:
  struct Peer {
    u32 ip = 0;  // where it is (host byte order): this computer's loopback when it is here
    u16 listening = 0;
    u32 nonce = 0;  // its random number: the number is its until it leaves or goes quiet
    std::array<u32, 4> addresses{};  // the addresses its beacons came from (its connections must too)
    Clock::time_point seen{};
    bool from(u32 a) const { return std::find(addresses.begin(), addresses.end(), a) != addresses.end(); }
  };

  explicit Link(const LinkOptions& options)
      : options_(options), game_(game_tag(options.game)), nonce_(std::random_device{}()) {
    thread_ = std::thread([this] { run(); });
  }
  ~Link() { stop(); }

  // Says goodbye to the other cabinets and stops the thread.
  void stop() {
    std::call_once(stopped_, [this] {
      {
        std::lock_guard lock(mutex_);
        stop_ = true;
      }
      wake_.notify_all();
      thread_.join();
    });
  }

  const LinkOptions& options() const { return options_; }
  const GameTag& game() const { return game_; }

  // From the game's thread: its number (0 for none yet), the ports it
  // listens on (bit k for 5000 + k) and the cabinets it has connected to and
  // that have connected to it (bit n for cabinet n).
  void set_cabinet(int cabinet) {
    std::lock_guard lock(mutex_);
    cabinet_ = cabinet;
  }
  void set_state(u16 listening, u8 outgoing, u8 incoming) {
    {
      std::lock_guard lock(mutex_);
      if (listening != listening_) changed_ = true;  // tell the others at once
      listening_ = listening;
      outgoing_ = outgoing;
      incoming_ = incoming;
    }
    wake_.notify_all();
  }
  // A cabinet heard from recently.
  bool peer(int cabinet, Peer* out) const {
    std::lock_guard lock(mutex_);
    if (!found(cabinet, Clock::now())) return false;
    *out = peers_[cabinet];
    return true;
  }

  LinkStatus status() const {
    std::lock_guard lock(mutex_);
    const Clock::time_point now = Clock::now();
    LinkStatus s;
    s.on = true;
    s.cabinet = cabinet_;
    for (int n = 1; n <= kCabinets; ++n) {
      s.found[n] = n != cabinet_ && found(n, now);
      s.connected[n] = ((outgoing_ | incoming_) >> n) & 1;
      s.linked[n] = ((outgoing_ & incoming_) >> n) & 1;
    }
    s.clash = clash_seen_ != Clock::time_point{} && now - clash_seen_ < kClashTimeout;
    s.error = error_;
    for (const auto& [ip, other] : incompatible_)
      if (now - other.seen < kPeerTimeout) s.incompatible.push_back(ipv4_string(ip) + " (" + other.version + ")");
    return s;
  }

 private:
  bool found(int cabinet, Clock::time_point now) const {
    return peers_[cabinet].seen != Clock::time_point{} && now - peers_[cabinet].seen < kPeerTimeout;
  }

  void run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
      const Clock::time_point now = Clock::now();
      const int cabinet = cabinet_;
      const u16 listening = listening_;
      const bool changed = changed_;
      changed_ = false;
      lock.unlock();
      if (cabinet && socket_cabinet_ != cabinet && now >= next_open_) open(cabinet, now);
      if (socket_ != kNoSocket) {
        receive();
        if (changed || now >= next_beacon_) {
          send(listening, false);
          next_beacon_ = now + kBeaconInterval;
        }
      }
      lock.lock();
      wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stop_ || changed_; });
    }
    lock.unlock();
    if (socket_ != kNoSocket) {
      send(0, true);  // so the others know at once
      close_host(socket_);
    }
  }

  void open(int cabinet, Clock::time_point now) {
    if (socket_ != kNoSocket) close_host(socket_), socket_ = kNoSocket;
    socket_cabinet_ = 0;
    const u16 port = beacon_port(options_.port, cabinet);
    const HostSocket b = open_host(SOCK_DGRAM);
    if (b == kNoSocket) return;
    const int on = 1;
    if (options_.lan) ::setsockopt(b, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef _WIN32
    // Windows reports a beacon sent to a cabinet that is not there as an
    // error on the next receive.
    BOOL report = FALSE;
    DWORD bytes = 0;
    WSAIoctl(b, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    const sockaddr_in a = host_address({options_.lan ? INADDR_ANY : INADDR_LOOPBACK, port});
    std::string error;
    if (::bind(b, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0) {
      socket_ = b;
      socket_cabinet_ = cabinet;
      net_log("link: cabinet %d, beacons on port %u%s", cabinet, port, options_.lan ? " (local network)" : "");
    } else {
      close_host(b);
      error = "Port " + std::to_string(port) + " is in use: is another OpenGP playing cabinet " +
              std::to_string(cabinet) + "?";
      net_log("link: %s", error.c_str());
      next_open_ = now + std::chrono::seconds(2);
    }
    std::lock_guard lock(mutex_);
    error_ = std::move(error);
  }

  void receive() {
    for (int i = 0; i < 64; ++i) {
      u8 data[128];
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const auto r = ::recvfrom(socket_, reinterpret_cast<char*>(data), sizeof(data), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
      if (r < 0) {
        if (would_block(host_error())) break;
        continue;
      }
      const Clock::time_point now = Clock::now();
      const u32 ip = ntohl(from.sin_addr.s_addr);
      // Only from this computer, or with linking on the local network, from
      // an address on one: never from the internet.
      if (!is_loopback(ip) && !(options_.lan && is_local_network(ip))) continue;
      u8 version = 0;
      if (!beacon_magic(data, static_cast<std::size_t>(r), &version)) continue;
      if (version != kLinkVersion) {
        // Another version of OpenGP: it cannot link with this one, but the
        // player should know it is there.
        std::string name = "an older OpenGP";
        if (version > kLinkVersion) name = "a newer OpenGP";
        if (version >= 2 && static_cast<std::size_t>(r) >= kBeaconVersionAt + kBeaconVersionSize) {
          const char* v = reinterpret_cast<const char*>(data + kBeaconVersionAt);
          name = "OpenGP " + std::string(v, strnlen(v, kBeaconVersionSize));
        }
        std::lock_guard lock(mutex_);
        if (!incompatible_.count(ip)) net_log("link: %s at %s cannot link with this one", name.c_str(), ipv4_string(ip).c_str());
        if (incompatible_.size() < 16 || incompatible_.count(ip)) incompatible_[ip] = {name, now};
        continue;
      }
      Beacon b;
      if (!read_beacon(data, static_cast<std::size_t>(r), game_, &b) || b.nonce == nonce_) continue;
      std::lock_guard lock(mutex_);
      if (b.cabinet == socket_cabinet_) {
        if (!b.leaving) clash_seen_ = now;
        continue;
      }
      Peer& p = peers_[b.cabinet];
      const bool known = found(b.cabinet, now);
      // A number belongs to the cabinet that has it until that one leaves or
      // goes quiet: another sender (a second cabinet with the same number, or
      // someone else on the network) cannot take it over or send it away.
      if (known && p.nonce != b.nonce) continue;
      if (b.leaving) {
        if (known) net_log("link: cabinet %d left", b.cabinet);
        p = {};
        continue;
      }
      if (!known) {
        net_log("link: found cabinet %d at %s", b.cabinet, ipv4_string(ip).c_str());
        p = {};
        p.nonce = b.nonce;
      }
      // A cabinet on this computer is heard both on the loopback interface
      // and by broadcast: connect to it on the loopback interface.
      if (!known || !is_loopback(p.ip) || is_loopback(ip)) p.ip = ip;
      if (!p.from(ip)) {
        const auto slot = std::find(p.addresses.begin(), p.addresses.end(), 0u);
        if (slot != p.addresses.end()) *slot = ip;
      }
      p.listening = b.listening;
      p.seen = now;
    }
  }

  // To the other cabinets on this computer, and with linking on the local
  // network to every cabinet's port there (this one's too: a cabinet with
  // the same number answers), by broadcast and to each cabinet found there
  // (broadcasts get lost, on Wi-Fi especially).
  void send(u16 listening, bool leaving) {
    u8 data[kBeaconSize];
    write_beacon(data, {socket_cabinet_, listening, nonce_, leaving}, game_, options_.version);
    auto send_to = [&](u32 ip, u16 port) {
      const sockaddr_in to = host_address({ip, port});
      ::sendto(socket_, reinterpret_cast<const char*>(data), sizeof(data), 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof(to));
    };
    const Clock::time_point now = Clock::now();
    if (options_.lan && now >= next_networks_) {
      broadcasts_ = broadcast_addresses();  // networks come and go
      next_networks_ = now + std::chrono::seconds(10);
    }
    std::array<u32, kCabinets + 1> found_at{};
    {
      std::lock_guard lock(mutex_);
      for (int n = 1; n <= kCabinets; ++n)
        if (found(n, now)) found_at[n] = peers_[n].ip;
    }
    for (int n = 1; n <= kCabinets; ++n) {
      const u16 port = beacon_port(options_.port, n);
      if (n != socket_cabinet_) send_to(INADDR_LOOPBACK, port);
      if (!options_.lan) continue;
      for (const u32 broadcast : broadcasts_) send_to(broadcast, port);
      if (found_at[n] && (found_at[n] >> 24) != 127) send_to(found_at[n], port);
    }
  }

  const LinkOptions options_;
  const GameTag game_;
  const u32 nonce_;

  mutable std::mutex mutex_;  // for what follows, up to the thread's own
  std::condition_variable wake_;
  bool stop_ = false;
  int cabinet_ = 0;
  u16 listening_ = 0;
  u8 outgoing_ = 0, incoming_ = 0;
  bool changed_ = false;
  std::array<Peer, kCabinets + 1> peers_{};
  struct Incompatible {
    std::string version;
    Clock::time_point seen;
  };
  std::map<u32, Incompatible> incompatible_;  // by address
  Clock::time_point clash_seen_{};
  std::string error_;

  // The thread's own.
  HostSocket socket_ = kNoSocket;  // bound to this cabinet's beacon port
  int socket_cabinet_ = 0;         // ... for this number
  std::vector<u32> broadcasts_;    // where the beacons go on the local network
  Clock::time_point next_beacon_{}, next_open_{}, next_networks_{};
  std::thread thread_;
  std::once_flag stopped_;
};

Network::Network(std::function<std::span<u8>(u32)> memory) : memory_(std::move(memory)) {
#ifdef _WIN32
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

Network::~Network() {
  for (auto& s : sockets_) s.reset();
  link_.reset();
#ifdef _WIN32
  WSACleanup();
#endif
}

void Network::add_service(Endpoint address, std::shared_ptr<Service> service) { services_[address] = std::move(service); }

void Network::add_route(Endpoint address, Endpoint to) { routes_[address] = to; }

void Network::enable_link(const LinkOptions& options) {
  link_ = std::make_unique<Link>(options);
  link_->set_cabinet(own_cabinet());
}

LinkStatus Network::link_status() const { return link_ ? link_->status() : LinkStatus{}; }

void Network::stop_link() {
  if (link_) link_->stop();
}

int Network::own_cabinet() const { return cabinet_at(own_address_); }

// Tells the link what the game listens on and who it is connected with.
void Network::update_link() {
  if (!link_) return;
  u16 listening = 0;
  u8 outgoing = 0, incoming = 0;
  for (const auto& s : sockets_) {
    if (!s || s->host == kNoSocket) continue;
    if (s->link_listener && s->listening) listening |= static_cast<u16>(1u << (s->game_port - kGamePort));
    if (s->cabinet) (s->accepted ? incoming : outgoing) |= static_cast<u8>(1u << s->cabinet);
  }
  link_->set_state(listening, outgoing, incoming);
}

// Takes the connections waiting on a link listener and reads their hellos.
void Network::take_incoming(Socket& listener) {
  const Clock::time_point now = Clock::now();
  for (int i = 0; i < 4; ++i) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const HostSocket h = ::accept(listener.host, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (h == kNoSocket) break;
    // A few cabinets connect at a time: anything past that is turned away.
    if (listener.incoming.size() >= kMaxIncoming) {
      net_log("link: too many connections waiting; one from %s closed", ipv4_string(ntohl(from.sin_addr.s_addr)).c_str());
      close_host(h);
      continue;
    }
    set_nonblocking(h);
    set_no_inherit(h);
    set_no_delay(h);
    Socket::Incoming in;
    in.host = h;
    in.from = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
    in.since = now;
    listener.incoming.push_back(in);
  }
  for (auto it = listener.incoming.begin(); it != listener.incoming.end();) {
    Socket::Incoming& in = *it;
    bool drop = false;
    if (!in.cabinet) {
      const auto r = ::recv(in.host, reinterpret_cast<char*>(in.hello.data() + in.received),
                            static_cast<int>(kHelloSize - in.received), 0);
      if (r > 0) in.received += static_cast<std::size_t>(r);
      else if (r == 0 || !would_block(host_error())) drop = true;
      if (in.received == kHelloSize) {
        in.cabinet = read_hello(in.hello.data(), link_->game());
        if (!in.cabinet || in.cabinet == own_cabinet()) {
          net_log("link: a connection from %s is not another cabinet of this game", ipv4_string(in.from.ip).c_str());
          drop = true;
        }
      } else if (!drop && now - in.since > kHelloTimeout) {
        drop = true;
      }
    }
    if (!drop && in.cabinet && !in.ready) {
      // The cabinet it says it is must be one whose beacons came from that
      // address (or one on this computer); until it is known, it waits.
      Link::Peer peer;
      if (link_->peer(in.cabinet, &peer)) {
        in.ready = peer.from(in.from.ip) || is_loopback(in.from.ip);
        if (!in.ready) {
          net_log("link: a connection from %s says it is cabinet %d, which is elsewhere", ipv4_string(in.from.ip).c_str(),
                  in.cabinet);
          drop = true;
        }
      } else if (now - in.since > kHelloTimeout) {
        drop = true;
      }
    }
    if (!drop && in.ready && now - in.since > kAcceptTimeout) drop = true;  // the game never took it
    if (drop) {
      close_host(in.host);
      it = listener.incoming.erase(it);
    } else {
      ++it;
    }
  }
}

Network::Socket* Network::find(u32 fd) { return fd < kSockets ? sockets_[fd].get() : nullptr; }

bool Network::sends_without_delay(u32 fd) const {
  const Socket* s = fd < kSockets ? sockets_[fd].get() : nullptr;
  if (!s || s->host == kNoSocket) return false;
  int on = 0;
  socklen_t size = sizeof(on);
  return ::getsockopt(s->host, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&on), &size) == 0 && on != 0;
}

u32 Network::fail(u32 error) {
  last_error_ = error;
  return kError;
}

std::string_view Network::string_at(u32 address, std::size_t max_length) {
  const std::span<u8> s = memory_(address);
  const std::size_t n = std::min(max_length, s.size());
  const char* p = reinterpret_cast<const char*>(s.data());
  return {p, p ? strnlen(p, n) : 0};
}

// Socket numbers go round rather than reusing the lowest free one, as in
// Dolphin: a number closed by one side of the game may still be in use by
// another for a moment.
u32 Network::new_socket() {
  for (std::size_t tries = 0; tries < kSockets; ++tries) {
    const u32 fd = next_fd_;
    next_fd_ = next_fd_ + 1 < kSockets ? next_fd_ + 1 : 1;
    if (!sockets_[fd]) return fd;
  }
  return 0;
}

u32 Network::execute(u16 command, std::span<const u32> p) {
  auto param = [&p](std::size_t i) { return i < p.size() ? p[i] : 0u; };
  switch (command) {
    case NET_SOCKET: return socket(param(0), param(1));
    case NET_CLOSESOCKET: return closesocket(param(0));
    case NET_CONNECT: return connect(param(0), param(1), param(2));
    case NET_BIND: return bind(param(0), param(1), param(2));
    case NET_LISTEN: return listen(param(0), param(1));
    case NET_ACCEPT: return accept(param(0), param(1), param(2));
    case NET_SEND: return send(param(0), param(1), param(2));
    case NET_RECV: return recv(param(0), param(1), param(2));
    case NET_SELECT: return select(param(0), param(1), param(2), param(3), param(4));
    case NET_SET_SOCK_OPT: return setsockopt(param(0), param(1), param(2), param(3), param(4));
    case NET_SET_TIMEOUTS: return set_timeouts(param(0), param(1), param(2), param(3));
    case NET_INET_ADDR: return inet_addr(param(0));
    case NET_MODIFY_MY_IP_ADDR: modify_my_ip_addr(param(0)); return 0;
    case NET_GET_LAST_ERROR: return last_error_;
    case NET_GET_PARAM_BY_DHCP_EXEC: return 0;
    default: net_log("unknown command %04X", command); return fail(SSC_EBADF);
  }
}

u32 Network::socket(u32 domain, u32 type) {
  const int host_type = type == kGuestDatagram ? SOCK_DGRAM : SOCK_STREAM;
  const u32 fd = new_socket();
  if (fd == 0 || domain != kGuestFamilyInet || (type != kGuestStream && type != kGuestDatagram)) {
    net_log("socket(%u, %u): no", domain, type);
    return fail(fd == 0 ? SSC_EMFILE : SSC_EBADF);
  }
  auto s = std::make_unique<Socket>();
  s->type = type;
  s->host = open_host(host_type);
  if (s->host == kNoSocket) {
    net_log("socket(%u, %u): host error %d", domain, type, host_error());
    return fail(SSC_EMFILE);
  }
  sockets_[fd] = std::move(s);
  net_log("socket(%u, %u) = %u", domain, type, fd);
  last_error_ = SSC_SUCCESS;
  return fd;
}

u32 Network::closesocket(u32 fd) {
  if (const Socket* s = find(fd); s && (s->sent || s->received))
    net_log("closesocket(%u): %llu bytes sent, %llu received", fd, static_cast<unsigned long long>(s->sent),
            static_cast<unsigned long long>(s->received));
  else
    net_log("closesocket(%u)%s", fd, s ? "" : ": not open");
  last_error_ = SSC_SUCCESS;
  if (!find(fd)) return kError;
  sockets_[fd].reset();
  update_link();
  return 0;
}

u32 Network::connect(u32 fd, u32 address, u32 length) {
  Socket* s = find(fd);
  const std::span<u8> a = memory_(address);
  if (!s) return fail(SSC_EBADF);
  if (length != kGuestAddressSize || a.size() < kGuestAddressSize) return fail(SSC_EFAULT);
  const Endpoint to = read_address(a.data());
  s->peer = to;

  if (auto service = services_.find(to); service != services_.end()) {
    // The service answers: the host socket is not needed.
    if (s->host != kNoSocket) close_host(s->host), s->host = kNoSocket;
    s->service = service->second->accept();
    net_log("connect(%u, %s:%u): service", fd, ipv4_string(to.ip).c_str(), to.port);
    last_error_ = SSC_SUCCESS;
    return 0;
  }
  if (s->host == kNoSocket) {
    net_log("connect(%u, %s:%u): refused (socket already used)", fd, ipv4_string(to.ip).c_str(), to.port);
    return fail(SSC_ECONNREFUSED);
  }
  if (const auto route = routes_.find(to); route != routes_.end()) {
    if (const u32 r = host_connect(*s, fd, to, route->second); r != SSC_SUCCESS) return fail(r);
    linked_ = true;
    last_error_ = SSC_SUCCESS;
    return 0;
  }

  // Another cabinet: only on a port it has said it listens on.
  if (const int cabinet = cabinet_at(to.ip); link_ && cabinet && is_game_port(to.port)) {
    const int own = own_cabinet();
    Link::Peer peer;
    if (cabinet == own || !link_->peer(cabinet, &peer) || !(peer.listening & (1u << (to.port - kGamePort)))) {
      net_log("connect(%u, %s:%u): refused (cabinet %d is not listening there)", fd, ipv4_string(to.ip).c_str(),
              to.port, cabinet);
      return fail(SSC_ECONNREFUSED);
    }
    // Whatever the host says, the game hears "refused": after one, it tries
    // again later, but after another error (a network hiccup, or macOS's
    // Local Network permission not given yet) it gives up on that cabinet.
    const Endpoint host{peer.ip, host_port(link_->options().port, cabinet, to.port)};
    if (host_connect(*s, fd, to, host, kLinkConnectWaitMs) != SSC_SUCCESS) return fail(SSC_ECONNREFUSED);
    set_no_delay(s->host);
    u8 hello[kHelloSize];
    write_hello(hello, own, link_->game());
    if (::send(s->host, reinterpret_cast<const char*>(hello), static_cast<int>(kHelloSize), kSendFlags) !=
        static_cast<int>(kHelloSize)) {
      net_log("connect(%u, %s:%u): the hello was not sent", fd, ipv4_string(to.ip).c_str(), to.port);
      return fail(SSC_ENOTCONN);
    }
    s->cabinet = cabinet;
    update_link();
    linked_ = true;
    last_error_ = SSC_SUCCESS;
    return 0;
  }

  net_log("connect(%u, %s:%u): refused (no route)", fd, ipv4_string(to.ip).c_str(), to.port);
  return fail(SSC_ECONNREFUSED);
}

// A host connection for the game's connection to `to`: waits for it, up to
// the game's timeout. Returns SSC_SUCCESS or the error.
u32 Network::host_connect(Socket& s, u32 fd, Endpoint to, Endpoint host, u32 max_wait_ms) {
  const sockaddr_in a = host_address(host);
  if (::connect(s.host, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) {
    const int error = host_error();
    if (!would_block(error)) {
      net_log("connect(%u, %s:%u): host error %d", fd, ipv4_string(to.ip).c_str(), to.port, error);
      return status_for(error);
    }
    PollFd pfd{};
    pfd.fd = s.host;
    pfd.events = POLLOUT;
    // The game's thread waits (holding the hardware), so never for long.
    const int ready = host_poll(&pfd, 1, static_cast<int>(std::min<u32>({s.connect_timeout_ms, static_cast<u32>(kMaxWaitMs), max_wait_ms})));
    if (ready <= 0) {
      net_log("connect(%u, %s:%u): timed out", fd, ipv4_string(to.ip).c_str(), to.port);
      return SSC_ETIMEDOUT;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s.host, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (so_error != 0) {
      net_log("connect(%u, %s:%u): host error %d", fd, ipv4_string(to.ip).c_str(), to.port, so_error);
      return status_for(so_error);
    }
  }
  net_log("connect(%u, %s:%u) via %s:%u", fd, ipv4_string(to.ip).c_str(), to.port, ipv4_string(host.ip).c_str(),
          host.port);
  return SSC_SUCCESS;
}

// On the loopback interface, so nothing outside this computer can reach the
// game, except for the link's ports with linking on the local network. A
// link port is this cabinet's host port for it (see LinkOptions).
u32 Network::bind(u32 fd, u32 address, u32 length) {
  Socket* s = find(fd);
  const std::span<u8> a = memory_(address);
  if (!s || s->host == kNoSocket) return fail(SSC_EBADF);
  if (length != kGuestAddressSize || a.size() < kGuestAddressSize) return fail(SSC_EFAULT);
  const Endpoint requested = read_address(a.data());
  Endpoint on{INADDR_LOOPBACK, requested.port};
  if (const int own = own_cabinet(); link_ && own && is_game_port(requested.port)) {
    on = {link_->options().lan ? INADDR_ANY : INADDR_LOOPBACK, host_port(link_->options().port, own, requested.port)};
    s->link_listener = true;
    s->game_port = requested.port;
#ifndef _WIN32
    // So a game started again at once can listen there while the last run's
    // connections wait out their close. (On Windows it would let two listen.)
    const int on_ = 1;
    ::setsockopt(s->host, SOL_SOCKET, SO_REUSEADDR, &on_, sizeof(on_));
#endif
  }
  const sockaddr_in host = host_address(on);
  const int r = ::bind(s->host, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  const int error = r == 0 ? 0 : host_error();
  net_log("bind(%u, %s:%u) on %s:%u: %s", fd, ipv4_string(requested.ip).c_str(), requested.port,
          ipv4_string(on.ip).c_str(), on.port, r == 0 ? "ok" : "failed");
  last_error_ = SSC_SUCCESS;
  if (r != 0) return fail(status_for(error));
  return 0;
}

u32 Network::listen(u32 fd, u32 backlog) {
  Socket* s = find(fd);
  if (!s || s->host == kNoSocket) return fail(SSC_EBADF);
  const int r = ::listen(s->host, static_cast<int>(std::clamp<u32>(backlog, 1, 16)));
  net_log("listen(%u, %u): %s", fd, backlog, r == 0 ? "ok" : "failed");
  if (r != 0) return fail(status_for(host_error()));
  s->listening = true;
  update_link();  // tell the other cabinets
  last_error_ = SSC_SUCCESS;
  return 0;
}

u32 Network::accept(u32 fd, u32 address, u32 length_address) {
  Socket* s = find(fd);
  if (!s || s->host == kNoSocket) return fail(SSC_EBADF);
  HostSocket h = kNoSocket;
  Endpoint from;
  int cabinet = 0;
  if (s->link_listener) {
    // Another cabinet, once its hello has come; it is at its game address.
    take_incoming(*s);
    const auto in = std::find_if(s->incoming.begin(), s->incoming.end(), [](const auto& i) { return i.ready; });
    if (in == s->incoming.end()) return fail(SSC_EWOULDBLOCK);
    h = in->host;
    cabinet = in->cabinet;
    from = {kFirstCabinet + static_cast<u32>(cabinet - 1), in->from.port};
    s->incoming.erase(in);
  } else {
    sockaddr_in a{};
    socklen_t a_len = sizeof(a);
    h = ::accept(s->host, reinterpret_cast<sockaddr*>(&a), &a_len);
    if (h == kNoSocket) return fail(status_for(host_error()));  // usually nobody waiting
    set_nonblocking(h);
    set_no_inherit(h);
    from = {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
  }
  const u32 client = new_socket();
  if (client == 0) {
    close_host(h);
    return fail(SSC_EMFILE);
  }
  auto c = std::make_unique<Socket>();
  c->host = h;
  c->peer = from;
  c->cabinet = cabinet;
  c->accepted = cabinet != 0;
  const std::span<u8> len = memory_(length_address);
  const std::span<u8> a = memory_(address);
  if (len.size() >= 4 && a.size() >= kGuestAddressSize) {
    write_address(a.data(), c->peer);
    put_le32(len.data(), kGuestAddressSize);
  }
  net_log("accept(%u) = %u from %s:%u", fd, client, ipv4_string(c->peer.ip).c_str(), c->peer.port);
  sockets_[client] = std::move(c);
  update_link();
  linked_ = true;
  last_error_ = SSC_SUCCESS;
  return client;
}

u32 Network::send(u32 fd, u32 data, u32 length) {
  Socket* s = find(fd);
  const std::span<u8> d = memory_(data);
  if (!s) return fail(SSC_EBADF);
  if (d.size() < length) return fail(SSC_EFAULT);
  if (s->service) {
    if (s->service->closed_) return fail(SSC_ENOTCONN);
    s->service->on_receive(d.first(length));
    last_error_ = SSC_SUCCESS;
    return length;
  }
  if (s->type != kGuestStream) {
    const auto r = ::send(s->host, reinterpret_cast<const char*>(d.data()), static_cast<int>(length), kSendFlags);
    if (r < 0) return fail(status_for(host_error()));
    last_error_ = SSC_SUCCESS;
    return static_cast<u32>(r);
  }
  // A stream: the game takes any answer as all of it sent (the link's
  // messages would lose their framing), so what the host cannot take now
  // waits here, in order, and goes with the next send, receive or select.
  if (const u32 error = flush(*s); error != SSC_SUCCESS) return fail(error);
  std::size_t done = 0;
  if (s->queued.empty()) {
    const auto r = ::send(s->host, reinterpret_cast<const char*>(d.data()), static_cast<int>(length), kSendFlags);
    if (r < 0 && !would_block(host_error())) return fail(status_for(host_error()));
    done = r > 0 ? static_cast<std::size_t>(r) : 0;
  }
  if (done < length) {
    if (s->queued.size() + (length - done) > kMaxQueued) {
      net_log("send(%u): the other end has not read for too long; closed", fd);
      close_host(s->host);
      s->host = kNoSocket;
      s->queued.clear();
      update_link();
      return fail(SSC_ENOTCONN);
    }
    s->queued.append(reinterpret_cast<const char*>(d.data()) + done, length - done);
  }
  if (log_enabled() && s->sent == 0 && length > 0) net_log("send(%u): the first %u bytes", fd, length);
  s->sent += length;
  last_error_ = SSC_SUCCESS;
  return length;
}

// Sends what a stream connection still holds, as much as the host takes.
u32 Network::flush(Socket& s) {
  while (!s.queued.empty() && s.host != kNoSocket) {
    const auto r = ::send(s.host, s.queued.data(), static_cast<int>(std::min<std::size_t>(s.queued.size(), 1 << 20)),
                          kSendFlags);
    if (r < 0) return would_block(host_error()) ? SSC_SUCCESS : status_for(host_error());
    s.queued.erase(0, static_cast<std::size_t>(r));
  }
  return SSC_SUCCESS;
}

u32 Network::recv(u32 fd, u32 data, u32 length) {
  Socket* s = find(fd);
  const std::span<u8> d = memory_(data);
  if (!s) return fail(SSC_EBADF);
  if (d.size() < length) return fail(SSC_EFAULT);
  if (s->service) {
    ServiceConnection& c = *s->service;
    const std::size_t n = std::min<std::size_t>(length, s->pending());
    if (n == 0) return c.closed_ ? (last_error_ = SSC_SUCCESS, 0u) : fail(SSC_EWOULDBLOCK);
    std::memcpy(d.data(), c.out_.data() + c.out_pos_, n);
    c.out_pos_ += n;
    last_error_ = SSC_SUCCESS;
    return static_cast<u32>(n);
  }
  if (s->host == kNoSocket) return fail(SSC_ENOTCONN);
  flush(*s);
  const auto r = ::recv(s->host, reinterpret_cast<char*>(d.data()), static_cast<int>(length), 0);
  if (r < 0) return fail(status_for(host_error()));
  if (log_enabled() && s->received == 0 && r > 0) net_log("recv(%u): the first %d bytes", fd, static_cast<int>(r));
  if (log_enabled() && r == 0) net_log("recv(%u): closed by the other end", fd);
  s->received += static_cast<u64>(r);
  last_error_ = SSC_SUCCESS;
  return static_cast<u32>(r);
}

u32 Network::select(u32 count, u32 read_set, u32 write_set, u32 except_set, u32 timeout) {
  count = std::min<u32>(count, kSockets);  // games may pass 256, the size of an fd_set
  const u32 sets[3] = {read_set, write_set, except_set};
  std::array<u8, kFdSetSize> in[3]{}, out[3]{};
  for (int k = 0; k < 3; ++k) {
    const std::span<u8> m = memory_(sets[k]);
    if (sets[k] && m.size() >= kFdSetSize) std::memcpy(in[k].data(), m.data(), kFdSetSize);
  }
  auto is_set = [](const std::array<u8, kFdSetSize>& set, u32 fd) { return (set[fd / 8] >> (fd % 8)) & 1; };
  auto mark = [](std::array<u8, kFdSetSize>& set, u32 fd) { set[fd / 8] |= static_cast<u8>(1 << (fd % 8)); };

  // Services are ready at once; host sockets are polled.
  u32 ready = 0;
  std::vector<PollFd> polled;
  std::vector<u32> polled_fd;
  for (u32 fd = 1; fd < count; ++fd) {
    const bool r = is_set(in[0], fd), w = is_set(in[1], fd), e = is_set(in[2], fd);
    if (!r && !w && !e) continue;
    Socket* s = find(fd);
    if (!s) continue;
    if (s->service) {
      if (r && (s->pending() || s->service->closed_)) mark(out[0], fd), ++ready;
      if (w && !s->service->closed_) mark(out[1], fd), ++ready;
      continue;
    }
    if (s->link_listener && s->listening) {
      // Ready once a cabinet's hello has come.
      take_incoming(*s);
      if (r && std::any_of(s->incoming.begin(), s->incoming.end(), [](const auto& i) { return i.ready; }))
        mark(out[0], fd), ++ready;
      continue;
    }
    if (s->host == kNoSocket) continue;
    flush(*s);
    PollFd p{};
    p.fd = s->host;
    p.events = static_cast<short>((r ? POLLIN : 0) | (w ? POLLOUT : 0) | (e ? kPollPri : 0));
    polled.push_back(p);
    polled_fd.push_back(fd);
  }

  int wait_ms = kMaxWaitMs;  // no timeout: wait (but not for ever)
  if (const std::span<u8> t = memory_(timeout); timeout && t.size() >= 8) {
    const u64 ms = u64{le32(t.data())} * 1000 + le32(t.data() + 4) / 1000;
    wait_ms = static_cast<int>(std::min<u64>(ms, kMaxWaitMs));
  }
  if (ready) wait_ms = 0;
  if (!polled.empty()) {
    if (host_poll(polled.data(), polled.size(), wait_ms) < 0) return fail(status_for(host_error()));
    for (std::size_t i = 0; i < polled.size(); ++i) {
      const short ev = polled[i].revents;
      const u32 fd = polled_fd[i];
      if (is_set(in[0], fd) && (ev & (POLLIN | POLLHUP | POLLERR))) mark(out[0], fd), ++ready;
      if (is_set(in[1], fd) && (ev & (POLLOUT | POLLERR))) mark(out[1], fd), ++ready;
      if (is_set(in[2], fd) && (ev & kPollPri) != 0) mark(out[2], fd), ++ready;
    }
  }

  for (int k = 0; k < 3; ++k) {
    const std::span<u8> m = memory_(sets[k]);
    if (sets[k] && m.size() >= kFdSetSize) std::memcpy(m.data(), out[k].data(), kFdSetSize);
  }
  last_error_ = SSC_SUCCESS;
  return ready;
}

u32 Network::setsockopt(u32 fd, u32 level, u32 name, u32 value, u32 length) {
  Socket* s = find(fd);
  if (!s) return fail(SSC_EBADF);
  const std::span<u8> v = memory_(value);
  const bool on = v.size() >= 4 && length >= 4 && le32(v.data()) != 0;
  int host_level = -1, host_name = -1;
  if (level == kGuestSolSocket && name == kGuestReuseAddr) host_level = SOL_SOCKET, host_name = SO_REUSEADDR;
  else if (level == kGuestSolSocket && name == kGuestKeepAlive) host_level = SOL_SOCKET, host_name = SO_KEEPALIVE;
  else if (level == kGuestSolSocket && name == kGuestBroadcast) host_level = SOL_SOCKET, host_name = SO_BROADCAST;
  else if (level == kGuestIpprotoTcp && name == kGuestTcpNoDelay) host_level = IPPROTO_TCP, host_name = TCP_NODELAY;
  net_log("setsockopt(%u, %X, %X, %u)%s", fd, level, name, on, host_name < 0 ? " (ignored)" : "");
  if (host_name >= 0 && s->host != kNoSocket) {
    const int flag = on;
    ::setsockopt(s->host, host_level, host_name, reinterpret_cast<const char*>(&flag), sizeof(flag));
  }
  last_error_ = SSC_SUCCESS;
  return 0;
}

// The game's timeouts are milliseconds (Mario Kart GP 2's camera uses 2000,
// its link 6000). Only connecting waits: sends and receives never block.
u32 Network::set_timeouts(u32 fd, u32 connect_ms, u32 send_ms, u32 recv_ms) {
  Socket* s = find(fd);
  net_log("settimeouts(%u, %u, %u, %u)", fd, connect_ms, send_ms, recv_ms);
  if (!s) return fail(SSC_EBADF);
  s->connect_timeout_ms = connect_ms;
  last_error_ = SSC_SUCCESS;
  return 0;
}

// The address in network byte order, as the game reads the reply word
// without swapping it (unlike other results).
u32 Network::inet_addr(u32 text) {
  const std::string_view s = string_at(text, 15);
  u32 ip = 0;
  if (!parse_ipv4(s, &ip)) {
    net_log("inet_addr(\"%.*s\"): not an address", static_cast<int>(s.size()), s.data());
    return fail(SSC_EFAULT);
  }
  last_error_ = SSC_SUCCESS;
  return (ip >> 24) | ((ip >> 8) & 0xFF00) | ((ip << 8) & 0xFF0000) | (ip << 24);
}

void Network::modify_my_ip_addr(u32 text) {
  const std::string_view s = string_at(text, 15);
  u32 ip = 0;
  if (parse_ipv4(s, &ip)) own_address_ = ip;
  if (link_) link_->set_cabinet(own_cabinet());
  net_log("modifymyipaddr(\"%.*s\")", static_cast<int>(s.size()), s.data());
}

}  // namespace triforce
