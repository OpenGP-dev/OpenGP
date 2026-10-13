// SPDX-License-Identifier: GPL-3.0-or-later

#include "discord/ipc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace discord {

namespace {

constexpr u32 kMaxLength = 64 * 1024;  // Discord's own limit for a message

void put_le32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 get_le32(const u8* p) {
  return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
         static_cast<u32>(p[3]) << 24;
}

// A byte stream to Discord with the messages framed on top.
class Stream : public Transport {
 public:
  bool connected() const override { return connected_; }

  void write(const Message& message) override {
    if (!connected_) return;
    const std::vector<u8> bytes = encode(message);
    if (!send_all(bytes.data(), bytes.size())) connected_ = false;
  }

  std::optional<Message> read(int timeout_ms) override {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      bool bad = false;
      if (auto message = decode(in_, &bad)) return message;
      if (bad) connected_ = false;
      if (!connected_) return std::nullopt;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
      u8 buf[4096];
      const long n = receive(buf, sizeof(buf), left > 0 ? static_cast<int>(left) : 0);
      if (n < 0) connected_ = false;
      else if (n == 0 && left <= 0) return std::nullopt;
      else in_.insert(in_.end(), buf, buf + n);
    }
  }

 protected:
  // Sends all of `data`; false if the connection has failed.
  virtual bool send_all(const u8* data, std::size_t size) = 0;
  // Receives what has arrived, waiting up to `timeout_ms` for something:
  // the byte count, 0 for nothing yet, or -1 if the connection has failed.
  virtual long receive(u8* buf, std::size_t size, int timeout_ms) = 0;

  bool connected_ = true;

 private:
  std::vector<u8> in_;
};

#ifdef _WIN32

class Pipe : public Stream {
 public:
  explicit Pipe(HANDLE handle) : handle_(handle) {}
  ~Pipe() override { CloseHandle(handle_); }

 protected:
  bool send_all(const u8* data, std::size_t size) override {
    while (size) {
      DWORD written = 0;
      if (!WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr)) return false;
      data += written, size -= written;
    }
    return true;
  }

  long receive(u8* buf, std::size_t size, int timeout_ms) override {
    // Named pipes opened without overlapped I/O cannot time out, so peek
    // at what has arrived and wait in short steps.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) return -1;
      if (available) {
        DWORD got = 0;
        if (!ReadFile(handle_, buf, static_cast<DWORD>(std::min<std::size_t>(size, available)), &got, nullptr))
          return -1;
        return static_cast<long>(got);
      }
      if (std::chrono::steady_clock::now() >= deadline) return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

 private:
  HANDLE handle_;
};

#else

class Socket : public Stream {
 public:
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket() override { ::close(fd_); }

 protected:
  bool send_all(const u8* data, std::size_t size) override {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;  // SO_NOSIGPIPE is set instead
#endif
    while (size) {
      const ssize_t n = ::send(fd_, data, size, kFlags);
      if (n <= 0) return false;
      data += n, size -= static_cast<std::size_t>(n);
    }
    return true;
  }

  long receive(u8* buf, std::size_t size, int timeout_ms) override {
    pollfd p{fd_, POLLIN, 0};
    const int ready = ::poll(&p, 1, timeout_ms);
    if (ready < 0) return -1;
    if (ready == 0) return 0;
    const ssize_t n = ::recv(fd_, buf, size, 0);
    return n > 0 ? static_cast<long>(n) : -1;  // 0: Discord closed the connection
  }

 private:
  int fd_;
};

#endif

}  // namespace

std::vector<u8> encode(const Message& message) {
  std::vector<u8> out;
  out.reserve(8 + message.json.size());
  put_le32(out, static_cast<u32>(message.op));
  put_le32(out, static_cast<u32>(message.json.size()));
  out.insert(out.end(), message.json.begin(), message.json.end());
  return out;
}

std::optional<Message> decode(std::vector<u8>& bytes, bool* bad) {
  if (bytes.size() < 8) return std::nullopt;
  const u32 op = get_le32(bytes.data());
  const u32 length = get_le32(bytes.data() + 4);
  if (op > static_cast<u32>(Opcode::Pong) || length > kMaxLength) {
    *bad = true;
    return std::nullopt;
  }
  if (bytes.size() < 8 + std::size_t{length}) return std::nullopt;
  Message message{static_cast<Opcode>(op), std::string(bytes.begin() + 8, bytes.begin() + 8 + length)};
  bytes.erase(bytes.begin(), bytes.begin() + 8 + length);
  return message;
}

std::unique_ptr<Transport> connect_local() {
#ifdef _WIN32
  for (int i = 0; i < 10; ++i) {
    const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return std::make_unique<Pipe>(h);
  }
#else
  // Where Discord puts its socket: the runtime directory, else the temporary
  // one; inside them, the Flatpak's or the Snap's directory for those builds.
  std::vector<std::string> dirs;
  for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
    if (const char* dir = std::getenv(var); dir && *dir) dirs.emplace_back(dir);
  }
  dirs.emplace_back("/tmp");
  for (const std::string& dir : dirs) {
    for (const char* sub : {"", "app/com.discordapp.Discord/", "snap.discord/"}) {
      for (int i = 0; i < 10; ++i) {
        const std::string path = dir + "/" + sub + "discord-ipc-" + std::to_string(i);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) continue;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
          return std::make_unique<Socket>(fd);
        ::close(fd);
      }
    }
  }
#endif
  return nullptr;
}

}  // namespace discord
