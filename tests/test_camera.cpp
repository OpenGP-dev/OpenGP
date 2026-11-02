// SPDX-License-Identifier: GPL-3.0-or-later
//
// The camera (camera/namcam.h, camera/jpeg.h) and the media board's network
// it answers on (triforce/network.h).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "camera/jpeg.h"
#include "camera/namcam.h"
#include "camera/exposure.h"
#include "triforce/network.h"

using namespace gekko;

namespace {

u16 be16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }

// The JPEG's segments in order (marker bytes), up to the scan.
std::vector<u8> segments(const std::vector<u8>& jpeg, u16* width, u16* height) {
  std::vector<u8> markers;
  std::size_t i = 2;
  while (i + 4 <= jpeg.size() && jpeg[i] == 0xFF) {
    const u8 m = jpeg[i + 1];
    markers.push_back(m);
    if (m == 0xC0) *height = be16(&jpeg[i + 5]), *width = be16(&jpeg[i + 7]);
    if (m == 0xDA) break;
    i += 2 + be16(&jpeg[i + 2]);
  }
  return markers;
}

// Media board memory for the network: one buffer at 0x1000.
struct Board {
  std::vector<u8> memory = std::vector<u8>(0x10000);
  triforce::Network net{[this](u32 a) {
    return a >= 0x1000 && a < 0x1000 + memory.size() ? std::span<u8>(memory).subspan(a - 0x1000) : std::span<u8>();
  }};
  u8* at(u32 a) { return &memory[a - 0x1000]; }
  u32 run(u16 command, std::initializer_list<u32> params) {
    const std::vector<u32> p(params);
    return net.execute(command, p);
  }
  void put_address(u32 a, u32 ip, u16 port) {
    u8* p = at(a);
    std::memset(p, 0, 16);
    p[1] = 2, p[2] = static_cast<u8>(port >> 8), p[3] = static_cast<u8>(port);
    p[4] = static_cast<u8>(ip >> 24), p[5] = static_cast<u8>(ip >> 16), p[6] = static_cast<u8>(ip >> 8), p[7] = static_cast<u8>(ip);
  }
};

constexpr u32 kError = 0xFFFFFFFF;

}  // namespace

TEST_CASE("the JPEG is baseline 4:2:0 with one table per segment") {
  std::vector<u8> rgb(camera::kPictureSize);
  camera::TestPattern().picture(rgb.data());
  const std::vector<u8> jpeg = camera::encode_jpeg(rgb.data(), camera::kWidth, camera::kHeight);
  REQUIRE(jpeg.size() > 1000);
  CHECK(jpeg.size() < 0x80000);  // the game's buffer
  CHECK(jpeg[0] == 0xFF);
  CHECK(jpeg[1] == 0xD8);
  CHECK(jpeg[jpeg.size() - 2] == 0xFF);  // the game checks the last two bytes
  CHECK(jpeg[jpeg.size() - 1] == 0xD9);
  u16 w = 0, h = 0;
  const std::vector<u8> expected = {0xE0, 0xDB, 0xDB, 0xC0, 0xC4, 0xC4, 0xC4, 0xC4, 0xDA};
  CHECK(segments(jpeg, &w, &h) == expected);
  CHECK(w == 320);
  CHECK(h == 240);
  if (const char* out = std::getenv("OPENGP_TEST_JPEG")) {  // for checking with another decoder
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }
}

TEST_CASE("ipv4 addresses") {
  u32 ip = 0;
  CHECK(triforce::parse_ipv4("192.168.29.104", &ip));
  CHECK(ip == 0xC0A81D68);
  CHECK(triforce::ipv4_string(ip) == "192.168.29.104");
  CHECK_FALSE(triforce::parse_ipv4("192.168.29", &ip));
  CHECK_FALSE(triforce::parse_ipv4("192.168.29.256", &ip));
  CHECK_FALSE(triforce::parse_ipv4("192.168.29.1x", &ip));
}

TEST_CASE("the game fetches a photo from the camera") {
  Board b;
  camera::attach(b.net, std::make_shared<camera::TestPattern>());

  const u32 fd = b.run(triforce::NET_SOCKET, {2, 1, 0});
  REQUIRE(fd >= 1);
  REQUIRE(fd < 64);
  b.put_address(0x1000, 0xC0A81D69, 80);  // PCB ID 2's camera
  CHECK(b.run(triforce::NET_CONNECT, {fd, 0x1000, 16}) == 0);

  // Nothing to read before the request.
  u8* set = b.at(0x1100);
  std::memset(set, 0, 32);
  set[fd / 8] = static_cast<u8>(1 << (fd % 8));
  CHECK(b.run(triforce::NET_SELECT, {256, 0x1100, 0, 0, 0x1200}) == 0);
  CHECK(set[fd / 8] == 0);

  const char request[] = "GET /img.jpg HTTP/1.0\r\n\r\n";
  std::memcpy(b.at(0x2000), request, sizeof(request) - 1);
  CHECK(b.run(triforce::NET_SEND, {fd, 0x2000, sizeof(request) - 1}) == sizeof(request) - 1);

  std::string reply;
  for (int reads = 0; reads < 100; ++reads) {
    set[fd / 8] = static_cast<u8>(1 << (fd % 8));
    REQUIRE(b.run(triforce::NET_SELECT, {256, 0x1100, 0, 0, 0x1200}) == 1);
    const u32 n = b.run(triforce::NET_RECV, {fd, 0x3000, 0x1000});
    REQUIRE(n != kError);
    if (n == 0) break;
    reply.append(reinterpret_cast<const char*>(b.at(0x3000)), n);
  }
  CHECK(reply.starts_with("HTTP/1.0 200 OK\r\n"));
  const std::size_t body = reply.find("\r\n\r\n");
  REQUIRE(body != std::string::npos);
  CHECK(static_cast<u8>(reply[body + 4]) == 0xFF);
  CHECK(static_cast<u8>(reply[body + 5]) == 0xD8);
  CHECK(static_cast<u8>(reply[reply.size() - 2]) == 0xFF);
  CHECK(static_cast<u8>(reply[reply.size() - 1]) == 0xD9);
  CHECK(b.run(triforce::NET_CLOSESOCKET, {fd}) == 0);
  CHECK_FALSE(b.net.linked());  // the camera is not another cabinet
}

TEST_CASE("connections without a route are refused") {
  Board b;
  const u32 fd = b.run(triforce::NET_SOCKET, {2, 1, 0});
  b.put_address(0x1000, 0xC0A81D97, 5008);  // another cabinet
  CHECK(b.run(triforce::NET_CONNECT, {fd, 0x1000, 16}) == kError);
  CHECK(b.run(triforce::NET_GET_LAST_ERROR, {fd}) == triforce::SSC_ECONNREFUSED);
  CHECK(b.run(triforce::NET_CLOSESOCKET, {fd}) == 0);
  CHECK(b.run(triforce::NET_CLOSESOCKET, {fd}) == kError);
  CHECK_FALSE(b.net.linked());
}

TEST_CASE("host sockets: a routed connection to a listening socket") {
  Board b;
  // A server socket, bound on the loopback interface.
  const u32 server = b.run(triforce::NET_SOCKET, {2, 1, 0});
  u16 port = 0;
  for (u16 p = 47100; p < 47200 && !port; ++p) {
    b.put_address(0x1000, 0, p);
    if (b.run(triforce::NET_BIND, {server, 0x1000, 16}) == 0) port = p;
  }
  REQUIRE(port != 0);
  CHECK(b.run(triforce::NET_LISTEN, {server, 5}) == 0);
  CHECK(b.run(triforce::NET_ACCEPT, {server, 0x1400, 0x1410}) == kError);  // nobody yet
  CHECK(b.run(triforce::NET_GET_LAST_ERROR, {server}) == triforce::SSC_EWOULDBLOCK);
  CHECK_FALSE(b.net.linked());

  // Another board (another cabinet) connecting to this one's address, routed
  // to the server.
  Board other;
  other.net.add_route({0xC0A81D97, 5000}, {0x7F000001, port});
  const u32 client = other.run(triforce::NET_SOCKET, {2, 1, 0});
  other.run(triforce::NET_SET_TIMEOUTS, {client, 2000, 2000, 2000});
  other.put_address(0x1000, 0xC0A81D97, 5000);
  REQUIRE(other.run(triforce::NET_CONNECT, {client, 0x1000, 16}) == 0);
  CHECK(other.net.linked());  // it connected to a cabinet
  CHECK_FALSE(b.net.linked());

  u32 accepted = kError;
  for (int tries = 0; tries < 200 && accepted == kError; ++tries) {
    std::memset(b.at(0x1410), 0, 4);
    b.at(0x1410)[0] = 16;
    accepted = b.run(triforce::NET_ACCEPT, {server, 0x1400, 0x1410});
  }
  REQUIRE(accepted != kError);
  CHECK(b.net.linked());  // a cabinet connected to it
  CHECK(b.at(0x1400)[1] == 2);  // AF_INET
  CHECK(b.at(0x1404)[0] == 127);

  std::memcpy(other.at(0x2000), "hello", 5);
  CHECK(other.run(triforce::NET_SEND, {client, 0x2000, 5}) == 5);
  u32 n = kError;
  for (int tries = 0; tries < 200 && n == kError; ++tries) {
    std::memset(b.at(0x1100), 0, 32);
    b.at(0x1100)[accepted / 8] = static_cast<u8>(1 << (accepted % 8));
    struct Timeout {
      u8 bytes[8] = {0, 0, 0, 0, 0x10, 0x27, 0, 0};  // 10 ms (little endian)
    } t;
    std::memcpy(b.at(0x1200), t.bytes, 8);
    if (b.run(triforce::NET_SELECT, {64, 0x1100, 0, 0, 0x1200}) == 1) n = b.run(triforce::NET_RECV, {accepted, 0x3000, 64});
  }
  REQUIRE(n == 5);
  CHECK(std::memcmp(b.at(0x3000), "hello", 5) == 0);
  CHECK(other.run(triforce::NET_CLOSESOCKET, {client}) == 0);
  for (u32 fd : {accepted, server}) CHECK(b.run(triforce::NET_CLOSESOCKET, {fd}) == 0);
  // Linked for the rest of the run, even with the connections closed.
  CHECK(b.net.linked());
  CHECK(other.net.linked());
}

TEST_CASE("InetAddr answers in network byte order") {
  Board b;
  std::memcpy(b.at(0x1000), "192.168.29.104", 15);
  const u32 r = b.run(triforce::NET_INET_ADDR, {0x1000, 14});
  u8 bytes[4];
  bytes[0] = static_cast<u8>(r), bytes[1] = static_cast<u8>(r >> 8), bytes[2] = static_cast<u8>(r >> 16), bytes[3] = static_cast<u8>(r >> 24);
  // Stored little endian in the reply, the bytes are the address in order.
  CHECK(bytes[0] == 192);
  CHECK(bytes[1] == 168);
  CHECK(bytes[2] == 29);
  CHECK(bytes[3] == 104);
}

// ---- A webcam's warm-up and brightness ----

TEST_CASE("a webcam's frames show once their brightness is steady") {
  using namespace std::chrono_literals;
  camera::Warmup w;
  w.start();
  const auto t0 = camera::Warmup::Clock::now();  // the first frame
  // Bright, then dark while the exposure adjusts: not yet.
  CHECK_FALSE(w.frame(34, t0));
  CHECK_FALSE(w.frame(7, t0 + 33ms));
  CHECK_FALSE(w.frame(40, t0 + 66ms));
  CHECK_FALSE(w.frame(90, t0 + 100ms));
  // Steady, but too soon after the first frame.
  CHECK_FALSE(w.frame(91, t0 + 133ms));
  CHECK_FALSE(w.frame(92, t0 + 166ms));
  CHECK_FALSE(w.frame(91, t0 + 200ms));
  CHECK_FALSE(w.frame(92, t0 + 233ms));
  // Steady and old enough.
  CHECK(w.frame(91, t0 + 266ms));
  CHECK(w.frame(10, t0 + 300ms));  // and from then on, whatever the picture
}

TEST_CASE("a webcam that never settles shows two seconds after its first frame") {
  using namespace std::chrono_literals;
  camera::Warmup w;
  w.start();
  const auto t0 = camera::Warmup::Clock::now() + 5s;  // opening it took a while
  CHECK_FALSE(w.frame(10, t0));
  CHECK_FALSE(w.frame(60, t0 + 1500ms));
  CHECK(w.frame(110, t0 + 2s));
}

TEST_CASE("mean brightness") {
  std::vector<u8> rgb(320 * 240 * 3, 200);
  CHECK(camera::mean_brightness(rgb.data(), 320 * 240) == doctest::Approx(200.0));
  std::fill(rgb.begin(), rgb.end(), u8{0});
  CHECK(camera::mean_brightness(rgb.data(), 320 * 240) == doctest::Approx(0.0));
}

TEST_CASE("a dim picture is brightened, gradually, and a bright one is left alone") {
  constexpr std::size_t kPixels = 320 * 240;
  camera::AutoGain gain;
  std::vector<u8> rgb(kPixels * 3, 20);
  // The first frame takes the gain it needs at once: 100 / 20 = 5.
  CHECK(gain.apply(rgb.data(), kPixels, 20) == doctest::Approx(5.0));
  CHECK(rgb[0] == 100);
  // Very dark: at most 6 times, approached gradually.
  std::fill(rgb.begin(), rgb.end(), u8{2});
  const double g = gain.apply(rgb.data(), kPixels, 2);
  CHECK(g > 5.0);
  CHECK(g < 6.0);
  // Bright enough: unchanged once the gain has come down.
  camera::AutoGain fresh;
  std::fill(rgb.begin(), rgb.end(), u8{150});
  CHECK(fresh.apply(rgb.data(), kPixels, 150) == doctest::Approx(1.0));
  CHECK(rgb[0] == 150);
}
