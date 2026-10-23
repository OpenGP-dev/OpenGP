// SPDX-License-Identifier: GPL-3.0-or-later
//
// The installer (tools/installer): reading a package's package.toml, from this
// release and from older ones, and downloading the compiler.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "installer/installer.h"
#include "test_temp.h"

namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A package folder with one game, whose package.toml's [build] ends with `build`.
fs::path package(const char* test, const std::string& build) {
  const fs::path home = opengp_test::temp_dir(test);
  write(home / "sdk" / "package.toml",
        "version = \"1.0.0\"\ngames = [\"mkgp2\"]\n\n[build]\ncxx = [\"c++\", \"g++\", \"clang++\"]\n"
        "link_group = true\n" + build);
  write(home / "sdk" / "games" / "mkgp2" / "game.toml",
        "id = \"GGPE02\"\ntitle = \"Mario Kart Arcade GP 2\"\nexecutable = \"opengp-mkgp2\"\n");
  return home;
}

using Socket = decltype(::socket(0, 0, 0));

void close_socket(Socket s) {
#ifdef _WIN32
  ::closesocket(s);
#else
  ::close(s);
#endif
}

// A web server on this computer that sends the whole of `body` for every
// request, as a server or a proxy that ignores requests to resume does.
class Server {
 public:
  explicit Server(std::string body) : body_(std::move(body)) {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(a);
    REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0);
    REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &size) == 0);
    REQUIRE(::listen(listener_, 4) == 0);
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~Server() {
    stop_ = true;
    thread_.join();
    close_socket(listener_);
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/compiler.zip"; }
  std::vector<std::string> requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }

 private:
  void serve() {
    while (!stop_) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(listener_, &readable);
      timeval wait{0, 20000};
      if (::select(static_cast<int>(listener_) + 1, &readable, nullptr, nullptr, &wait) <= 0) continue;
      const Socket s = ::accept(listener_, nullptr, nullptr);
      std::string request;
      char buffer[1024];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto n = ::recv(s, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<std::size_t>(n));
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
      }
      const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_.size()) +
                                   "\r\nConnection: close\r\n\r\n" + body_;
      ::send(s, response.data(), static_cast<int>(response.size()), 0);
      close_socket(s);
    }
  }

  std::string body_;
  Socket listener_;
  unsigned short port_ = 0;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<std::string> requests_;
  std::thread thread_;
};

// A package that downloads its compiler from `url`, with an interrupted
// download of it: one byte.
installer::Package downloading(const char* test, const std::string& url) {
  installer::Package p;
  p.home = opengp_test::temp_dir(test);
  p.sdk = p.home / "sdk";
  p.toolchain_url = url;
  p.toolchain_sha256 = "none";
  p.toolchain_dir = "compiler";
  p.toolchain_cxx = "c++";
  write(p.home / "toolchain" / "compiler.zip", "x");
  return p;
}

const installer::Reporter kQuiet{[](const std::string&) {}, [](double) {}};

}  // namespace

TEST_CASE("a package from 1.0.0, which lists no sources, builds 1.0.0's") {
  installer::Package p;
  std::string error;
  REQUIRE(installer::load_package(package("sources_1_0_0", ""), &p, &error));
  CHECK(p.common_sources == std::vector<std::string>{"main.cpp", "arcade.cpp", "cabinet.cpp", "camera_use.cpp",
                                                     "game_log.cpp", "sdk_hle.cpp"});
  CHECK(p.game_sources == std::vector<std::string>{"patches.cpp", "status.cpp"});
  REQUIRE(p.games.size() == 1);
  CHECK(p.games[0].id == "GGPE02");
}

TEST_CASE("a package's lists of sources are the ones built") {
  installer::Package p;
  std::string error;
  REQUIRE(installer::load_package(
      package("sources_listed", "common_sources = [\"main.cpp\", \"new.cpp\"]\ngame_sources = [\"patches.cpp\"]\n"),
      &p, &error));
  CHECK(p.common_sources == std::vector<std::string>{"main.cpp", "new.cpp"});
  CHECK(p.game_sources == std::vector<std::string>{"patches.cpp"});
}

TEST_CASE("a package with an empty list of sources is refused") {
  for (const char* build : {"common_sources = []\n", "game_sources = []\n",
                            "common_sources = [\"main.cpp\"]\ngame_sources = []\n"}) {
    CAPTURE(build);
    installer::Package p;
    std::string error;
    CHECK_FALSE(installer::load_package(package("sources_empty", build), &p, &error));
    CHECK(error.ends_with("package.toml: no game sources listed"));
  }
}

TEST_CASE("a compiler download that cannot be resumed starts again from the beginning") {
  // "abc", whose SHA-256 the error names: all of it came down.
  Server server("abc");
  const installer::Package p = downloading("download_restarted", server.url());
  std::string cxx, error;
  CHECK_FALSE(installer::find_compiler(p, "", kQuiet, &cxx, &error));
  CHECK(error == "the download's SHA-256 is ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad, "
                 "expected none");
  const std::vector<std::string> requests = server.requests();
  REQUIRE(requests.size() == 2);
  CHECK(requests[0].find("Range: bytes=1-") != std::string::npos);
  CHECK(requests[1].find("Range:") == std::string::npos);
}

TEST_CASE("a compiler download that fails otherwise is kept, to be resumed") {
  std::string url;
  {
    Server server("abc");
    url = server.url();
  }
  // Nothing listens there now.
  const installer::Package p = downloading("download_kept", url);
  std::string cxx, error;
  CHECK_FALSE(installer::find_compiler(p, "", kQuiet, &cxx, &error));
  CHECK(error.starts_with("cannot download " + url));
  CHECK(read(p.home / "toolchain" / "compiler.zip") == "x");
}
