// SPDX-License-Identifier: GPL-3.0-or-later
//
// A webcam as the camera's picture source, through SDL3's camera API. The
// webcam is on only while the game asks for pictures (the player's photo, the
// test menu's camera page): a request turns it on, and it goes off a few
// seconds after the last. The game's start-up check asks once, and gets a grey
// picture without the webcam. While it is on, a thread takes its frames as
// they come and keeps the newest, cropped to 4:3 around its centre and scaled
// down to 320x240. Until its exposure has settled (warmup.h; a fraction of a
// second to two seconds) there is no picture, and the camera sends a grey one.
//
// Some systems ask the player first (macOS): until they allow it, and if they
// don't, there are no pictures.
#pragma once

#include <chrono>
#include <condition_variable>
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

  // Chooses the webcam called `name`, or the first one for "" or if there is
  // none of that name. It stays off until the game asks for a picture. Call
  // on the main thread.
  bool choose(const std::string& name, std::string* error);
  const std::string& name() const { return name_; }

  // Also turns the webcam on, or keeps it on.
  bool picture(u8* rgb) override;

 private:
  void run();
  SDL_Camera* open();

  std::string name_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;  // the webcam is wanted, or the thread is to stop
  // Guarded by mutex_:
  bool stop_ = false;
  bool on_ = false;
  std::chrono::steady_clock::time_point asked_;  // the game's latest request
  std::vector<u8> latest_;  // kPictureSize bytes once there is a picture
  bool asked_before_ = false;  // the game has asked for a picture this run
};

}  // namespace camera
