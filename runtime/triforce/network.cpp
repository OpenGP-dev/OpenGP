// SPDX-License-Identifier: GPL-3.0-or-later

#include "triforce/network.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#else
#include <arpa/inet.h>
#include <fcntl.h>
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
constexpr int kSendFlags = 0;
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
#if defined(__linux__)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

// A new host socket for the guest: non-blocking, and never raising SIGPIPE.
HostSocket open_host(int type) {
  const HostSocket s = ::socket(AF_INET, type, 0);
  if (s == kNoSocket) return s;
  set_nonblocking(s);
#if defined(__APPLE__)
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  return s;
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

}  // namespace

#define net_log(...)                       \
  do {                                     \
    if (log_enabled()) {                   \
      std::fputs("[network] ", stderr);    \
      std::fprintf(stderr, __VA_ARGS__);   \
      std::fputc('\n', stderr);            \
    }                                      \
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
  HostSocket host = kNoSocket;
  std::unique_ptr<ServiceConnection> service;
  u32 type = kGuestStream;
  u32 connect_timeout_ms = 2000;
  Endpoint peer;  // the address the game connected to

  ~Socket() {
    if (host != kNoSocket) close_host(host);
  }
  // Bytes the service has for the game.
  std::size_t pending() const { return service ? service->out_.size() - service->out_pos_ : 0; }
};

Network::Network(std::function<std::span<u8>(u32)> memory) : memory_(std::move(memory)) {
#ifdef _WIN32
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

Network::~Network() {
  for (auto& s : sockets_) s.reset();
#ifdef _WIN32
  WSACleanup();
#endif
}

void Network::add_service(Endpoint address, std::shared_ptr<Service> service) { services_[address] = std::move(service); }

void Network::add_route(Endpoint address, Endpoint to) { routes_[address] = to; }

Network::Socket* Network::find(u32 fd) { return fd < kSockets ? sockets_[fd].get() : nullptr; }

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
  net_log("closesocket(%u)%s", fd, find(fd) ? "" : ": not open");
  last_error_ = SSC_SUCCESS;
  if (!find(fd)) return kError;
  sockets_[fd].reset();
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
  const auto route = routes_.find(to);
  if (route == routes_.end() || s->host == kNoSocket) {
    net_log("connect(%u, %s:%u): refused (no route)", fd, ipv4_string(to.ip).c_str(), to.port);
    return fail(SSC_ECONNREFUSED);
  }

  // A host connection: wait for it, up to the game's timeout.
  const sockaddr_in host = host_address(route->second);
  if (::connect(s->host, reinterpret_cast<const sockaddr*>(&host), sizeof(host)) != 0) {
    const int error = host_error();
    if (!would_block(error)) {
      net_log("connect(%u, %s:%u): host error %d", fd, ipv4_string(to.ip).c_str(), to.port, error);
      return fail(status_for(error));
    }
    PollFd pfd{};
    pfd.fd = s->host;
    pfd.events = POLLOUT;
    const int ready = host_poll(&pfd, 1, static_cast<int>(std::min<u32>(s->connect_timeout_ms, kMaxWaitMs)));
    if (ready <= 0) {
      net_log("connect(%u, %s:%u): timed out", fd, ipv4_string(to.ip).c_str(), to.port);
      return fail(SSC_ETIMEDOUT);
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s->host, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (so_error != 0) {
      net_log("connect(%u, %s:%u): host error %d", fd, ipv4_string(to.ip).c_str(), to.port, so_error);
      return fail(status_for(so_error));
    }
  }
  net_log("connect(%u, %s:%u) via %s:%u", fd, ipv4_string(to.ip).c_str(), to.port,
          ipv4_string(route->second.ip).c_str(), route->second.port);
  last_error_ = SSC_SUCCESS;
  return 0;
}

// Only the loopback interface for now: nothing outside this computer can
// reach the game until linked cabinets route the cabinets' addresses.
u32 Network::bind(u32 fd, u32 address, u32 length) {
  Socket* s = find(fd);
  const std::span<u8> a = memory_(address);
  if (!s || s->host == kNoSocket) return fail(SSC_EBADF);
  if (length != kGuestAddressSize || a.size() < kGuestAddressSize) return fail(SSC_EFAULT);
  const Endpoint requested = read_address(a.data());
  const sockaddr_in host = host_address({INADDR_LOOPBACK, requested.port});
  const int r = ::bind(s->host, reinterpret_cast<const sockaddr*>(&host), sizeof(host));
  const int error = r == 0 ? 0 : host_error();
  net_log("bind(%u, %s:%u) on 127.0.0.1: %s", fd, ipv4_string(requested.ip).c_str(), requested.port,
          r == 0 ? "ok" : "failed");
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
  last_error_ = SSC_SUCCESS;
  return 0;
}

u32 Network::accept(u32 fd, u32 address, u32 length_address) {
  Socket* s = find(fd);
  if (!s || s->host == kNoSocket) return fail(SSC_EBADF);
  sockaddr_in from{};
  socklen_t from_len = sizeof(from);
  const HostSocket h = ::accept(s->host, reinterpret_cast<sockaddr*>(&from), &from_len);
  if (h == kNoSocket) return fail(status_for(host_error()));  // usually nobody waiting
  const u32 client = new_socket();
  if (client == 0) {
    close_host(h);
    return fail(SSC_EMFILE);
  }
  set_nonblocking(h);
  auto c = std::make_unique<Socket>();
  c->host = h;
  c->peer = {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
  const std::span<u8> len = memory_(length_address);
  const std::span<u8> a = memory_(address);
  if (len.size() >= 4 && a.size() >= kGuestAddressSize) {
    write_address(a.data(), c->peer);
    put_le32(len.data(), kGuestAddressSize);
  }
  net_log("accept(%u) = %u from %s:%u", fd, client, ipv4_string(c->peer.ip).c_str(), c->peer.port);
  sockets_[client] = std::move(c);
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
  const auto r = ::send(s->host, reinterpret_cast<const char*>(d.data()), static_cast<int>(length), kSendFlags);
  if (r < 0) return fail(status_for(host_error()));
  last_error_ = SSC_SUCCESS;
  return static_cast<u32>(r);
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
  const auto r = ::recv(s->host, reinterpret_cast<char*>(d.data()), static_cast<int>(length), 0);
  if (r < 0) return fail(status_for(host_error()));
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
    if (s->host == kNoSocket) continue;
    PollFd p{};
    p.fd = s->host;
    p.events = static_cast<short>((r ? POLLIN : 0) | (w ? POLLOUT : 0) | (e ? POLLPRI : 0));
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
      if (is_set(in[2], fd) && (ev & POLLPRI)) mark(out[2], fd), ++ready;
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
  net_log("modifymyipaddr(\"%.*s\")", static_cast<int>(s.size()), s.data());
}

}  // namespace triforce
