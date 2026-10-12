// SPDX-License-Identifier: GPL-3.0-or-later

#include "camera/webcam.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <tuple>

namespace camera {

namespace {

bool start_sdl_camera() { return SDL_WasInit(SDL_INIT_CAMERA) || SDL_InitSubSystem(SDL_INIT_CAMERA); }

// The webcam's smallest mode that still holds 320x240 (less to convert),
// preferring 4:3 and at least 15 frames a second.
bool choose_mode(SDL_CameraID id, SDL_CameraSpec* chosen) {
  int count = 0;
  SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(id, &count);
  if (!specs) return false;
  auto score = [](const SDL_CameraSpec& s) {
    const long long area = static_cast<long long>(s.width) * s.height;
    const bool big_enough = s.width >= kWidth && s.height >= kHeight;
    const bool four_by_three = s.width * 3 == s.height * 4;
    const bool fast_enough = s.framerate_denominator > 0 && s.framerate_numerator >= 15 * s.framerate_denominator;
    // Big enough first, then fast enough, then the smallest, then 4:3.
    return std::make_tuple(big_enough ? 0 : 1, fast_enough ? 0 : 1, big_enough ? area : -area, four_by_three ? 0 : 1);
  };
  const SDL_CameraSpec* best = nullptr;
  for (int i = 0; i < count; ++i)
    if (!best || score(*specs[i]) < score(*best)) best = specs[i];
  if (best) *chosen = *best;
  SDL_free(specs);
  return best != nullptr;
}

// Crops `frame` (RGB24) to 4:3 around its centre and scales it to 320x240,
// averaging the pixels each output pixel covers.
void crop_and_scale(const SDL_Surface* frame, u8* out) {
  const int w = frame->w, h = frame->h;
  int cw = w, ch = h;
  if (w * 3 > h * 4) cw = h * 4 / 3;
  else ch = w * 3 / 4;
  const int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
  const auto* pixels = static_cast<const u8*>(frame->pixels);
  for (int oy = 0; oy < kHeight; ++oy) {
    const int sy0 = y0 + oy * ch / kHeight, sy1 = std::max(sy0 + 1, y0 + (oy + 1) * ch / kHeight);
    for (int ox = 0; ox < kWidth; ++ox) {
      const int sx0 = x0 + ox * cw / kWidth, sx1 = std::max(sx0 + 1, x0 + (ox + 1) * cw / kWidth);
      unsigned sum[3] = {0, 0, 0};
      for (int sy = sy0; sy < sy1; ++sy) {
        const u8* p = pixels + static_cast<std::size_t>(sy) * frame->pitch + static_cast<std::size_t>(sx0) * 3;
        for (int sx = sx0; sx < sx1; ++sx, p += 3) sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
      }
      const unsigned n = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
      u8* o = out + (static_cast<std::size_t>(oy) * kWidth + ox) * 3;
      for (int c = 0; c < 3; ++c) o[c] = static_cast<u8>(sum[c] / n);
    }
  }
}

}  // namespace

Webcam::~Webcam() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (camera_) SDL_CloseCamera(camera_);
}

std::vector<std::string> Webcam::names() {
  std::vector<std::string> names;
  if (!start_sdl_camera()) return names;
  int count = 0;
  if (SDL_CameraID* ids = SDL_GetCameras(&count)) {
    for (int i = 0; i < count; ++i)
      if (const char* n = SDL_GetCameraName(ids[i])) names.emplace_back(n);
    SDL_free(ids);
  }
  return names;
}

bool Webcam::open(const std::string& name, std::string* error) {
  if (!start_sdl_camera()) {
    *error = std::string("no camera support: ") + SDL_GetError();
    return false;
  }
  int count = 0;
  SDL_CameraID* ids = SDL_GetCameras(&count);
  SDL_CameraID id = 0;
  for (int i = 0; ids && i < count && !id; ++i)
    if (const char* n = SDL_GetCameraName(ids[i]); n && name == n) id = ids[i];
  if (!id && ids && count > 0) id = ids[0];
  SDL_free(ids);
  if (!id) {
    *error = "no webcam found";
    return false;
  }
  const char* n = SDL_GetCameraName(id);
  name_ = n ? n : "webcam";
  SDL_CameraSpec spec{};
  camera_ = SDL_OpenCamera(id, choose_mode(id, &spec) ? &spec : nullptr);
  if (!camera_) {
    *error = name_ + ": " + SDL_GetError();
    return false;
  }
  thread_ = std::thread([this] { run(); });
  return true;
}

bool Webcam::picture(u8* rgb) {
  std::lock_guard lock(mutex_);
  if (latest_.size() != kPictureSize) return false;
  std::memcpy(rgb, latest_.data(), kPictureSize);
  return true;
}

void Webcam::run() {
  std::vector<u8> picture(kPictureSize);
  bool first = true;
  while (!stop_) {
    // -1 denied, 0 not decided yet, 1 allowed (an int before SDL 3.4).
    const int permission = static_cast<int>(SDL_GetCameraPermissionState(camera_));
    if (permission < 0) {
      std::fprintf(stderr, "[camera] %s: access was denied, so there are no pictures\n", name_.c_str());
      return;
    }
    SDL_Surface* frame = permission > 0 ? SDL_AcquireCameraFrame(camera_, nullptr) : nullptr;
    if (!frame) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    SDL_Surface* converted = SDL_ConvertSurface(frame, SDL_PIXELFORMAT_RGB24);
    const int w = frame->w, h = frame->h;
    SDL_ReleaseCameraFrame(camera_, frame);
    if (!converted) continue;
    crop_and_scale(converted, picture.data());
    SDL_DestroySurface(converted);
    {
      std::lock_guard lock(mutex_);
      latest_ = picture;
    }
    if (first) std::fprintf(stderr, "[camera] %s: %dx%d\n", name_.c_str(), w, h);
    first = false;
  }
}

}  // namespace camera
