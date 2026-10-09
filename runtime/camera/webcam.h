// SPDX-License-Identifier: GPL-3.0-or-later
//
// A webcam as the camera's picture source, through SDL3's camera API. A
// thread takes the webcam's frames as they come and keeps the newest, cropped
// to 4:3 around its centre and scaled down to 320x240.
//
// Some systems ask the player first (macOS): until they allow it, and if they
// don't, there are no pictures.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "camera/namcam.h"

struct SDL_Camera;

namespace camera {

class Webcam final : public Source {
 public:
  Webcam() = default;
  ~Webcam() override;
  Webcam(const Webcam&) = delete;
  Webcam& operator=(const Webcam&) = delete;

  // The webcams' names. Starts SDL's camera support (call on the main thread).
  static std::vector<std::string> names();

  // Opens the webcam called `name`, or the first one for "" or if there is
  // none of that name, and starts taking pictures. Call on the main thread.
  bool open(const std::string& name, std::string* error);
  const std::string& name() const { return name_; }

  bool picture(u8* rgb) override;

 private:
  void run();

  SDL_Camera* camera_ = nullptr;
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::vector<u8> latest_;  // kPictureSize bytes once there is a picture
};

}  // namespace camera
