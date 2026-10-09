// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NamCam2, the cabinet's camera: an HTTP server on the media board's
// network that takes the player's photo. For a picture the game connects to
// the camera's port 80, sends "GET /img.jpg HTTP/1.0" and reads the reply up
// to the end of the connection: HTTP headers, then a 320x240 JPEG, which it
// decodes itself. OpenGP answers as the camera (a triforce::Service), with
// pictures from a webcam (camera/webcam.h) or a test pattern.
#pragma once

#include <memory>
#include <vector>

#include "gekko/types.h"
#include "triforce/network.h"

namespace camera {

using namespace gekko;

// The camera's pictures: 320x240, RGB.
constexpr int kWidth = 320, kHeight = 240;
constexpr std::size_t kPictureSize = std::size_t{kWidth} * kHeight * 3;

// Where the camera's pictures come from. Thread-safe.
class Source {
 public:
  virtual ~Source() = default;
  // Copies the newest picture to `rgb` (kPictureSize bytes); false if there
  // is none yet.
  virtual bool picture(u8* rgb) = 0;
};

// Colour bars, with a square that moves each second (to tell pictures apart).
class TestPattern final : public Source {
 public:
  bool picture(u8* rgb) override;
};

class NamCam final : public triforce::Service {
 public:
  explicit NamCam(std::shared_ptr<Source> source) : source_(std::move(source)) {}
  std::unique_ptr<triforce::ServiceConnection> accept() override;

  // The reply to a request for /img.jpg: headers and a JPEG.
  std::vector<u8> photo_reply();

 private:
  std::shared_ptr<Source> source_;
  std::vector<u8> rgb_ = std::vector<u8>(kPictureSize);
  bool warned_ = false;
};

// The cameras' addresses: 192.168.29.104 + (PCB ID - 1), for PCB IDs 1-4.
constexpr u32 kFirstCameraAddress = 0xC0A81D68;
constexpr int kCameras = 4;

// Puts a NamCam at every camera address, answering from `source`.
void attach(triforce::Network& network, std::shared_ptr<Source> source);

}  // namespace camera
