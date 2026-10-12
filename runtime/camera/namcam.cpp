// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera/namcam.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "camera/jpeg.h"

namespace camera {

namespace {

// One HTTP/1.0 exchange: a request, the reply, then the camera hangs up.
class Connection final : public triforce::ServiceConnection {
 public:
  explicit Connection(NamCam& camera) : camera_(camera) {}

  void on_receive(std::span<const u8> data) override {
    if (answered_) return;
    request_.append(reinterpret_cast<const char*>(data.data()), data.size());
    if (request_.find("\r\n\r\n") == std::string::npos) {
      if (request_.size() > 4096) answer("400 Bad Request");
      return;
    }
    const std::string_view line = std::string_view(request_).substr(0, request_.find("\r\n"));
    if (line.starts_with("GET /img.jpg ")) {
      answered_ = true;
      reply(camera_.photo_reply());
      close();
    } else {
      std::fprintf(stderr, "[camera] unexpected request: %.*s\n", static_cast<int>(line.size()), line.data());
      answer("404 Not Found");
    }
  }

 private:
  void answer(const char* status) {
    const std::string r = std::string("HTTP/1.0 ") + status + "\r\nContent-Length: 0\r\n\r\n";
    reply({reinterpret_cast<const u8*>(r.data()), r.size()});
    close();
    answered_ = true;
  }

  NamCam& camera_;
  std::string request_;
  bool answered_ = false;
};

}  // namespace

bool TestPattern::picture(u8* rgb) {
  static constexpr u8 kBars[8][3] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
                                     {192, 0, 192},   {192, 0, 0},   {0, 0, 192},   {16, 16, 16}};
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
  const int square_x = static_cast<int>(seconds % 8) * (kWidth / 8);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      u8* p = rgb + (static_cast<std::size_t>(y) * kWidth + x) * 3;
      const bool square = y >= kHeight * 3 / 4 && x >= square_x && x < square_x + kWidth / 8;
      if (y < kHeight * 3 / 4) std::memcpy(p, kBars[x * 8 / kWidth], 3);
      else std::memset(p, square ? 235 : static_cast<u8>(x * 255 / (kWidth - 1)) / 2, 3);
    }
  }
  return true;
}

std::unique_ptr<triforce::ServiceConnection> NamCam::accept() { return std::make_unique<Connection>(*this); }

std::vector<u8> NamCam::photo_reply() {
  if (!source_ || !source_->picture(rgb_.data())) {
    // No picture yet (the webcam is starting): grey.
    if (!warned_) std::fprintf(stderr, "[camera] no picture from the webcam yet: sending a grey one\n");
    warned_ = true;
    std::memset(rgb_.data(), 0x80, rgb_.size());
  }
  const std::vector<u8> jpeg = encode_jpeg(rgb_.data(), kWidth, kHeight);
  char header[128];
  const int n = std::snprintf(header, sizeof(header),
                              "HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", jpeg.size());
  std::vector<u8> reply(header, header + n);
  reply.insert(reply.end(), jpeg.begin(), jpeg.end());
  return reply;
}

void attach(triforce::Network& network, std::shared_ptr<Source> source) {
  auto camera = std::make_shared<NamCam>(std::move(source));
  for (int i = 0; i < kCameras; ++i) network.add_service({kFirstCameraAddress + static_cast<u32>(i), 80}, camera);
}

}  // namespace camera
