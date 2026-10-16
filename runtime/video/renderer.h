// SPDX-License-Identifier: GPL-3.0-or-later
//
// The renderer: draws gx::Frames with SDL3's GPU API (Vulkan, and later
// D3D12/Metal) and shows their external framebuffer in a window.
//
//   * The EFB is a colour + depth render target (640x528, times the
//     internal resolution scale; wider for widescreen, see gx/frame.h).
//   * Draws use one pipeline per blend/depth combination, all with the TEV
//     ubershader (shaders/tev.frag).
//   * EFB copies render into textures the frame's draws can then sample;
//     copies to the XFB are what gets presented.
//
// Rendering happens on the thread that owns the window (the main thread).
// Frames are produced on the guest thread and handed over by FrameQueue.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gx/frame.h"

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUShader;
struct SDL_GPUBuffer;
struct SDL_GPUTransferBuffer;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUFence;

namespace video {

using namespace gekko;

// An RGBA8 image (screenshots).
struct Image {
  u32 width = 0, height = 0;
  std::vector<u8> rgba;
};

// Hands frames from the guest thread to the render thread. wait_for_room()
// blocks while the renderer is `max_pending` frames behind, which paces the
// game.
class FrameQueue final : public gx::FrameOutput {
 public:
  explicit FrameQueue(std::size_t max_pending = 2) : max_pending_(max_pending) {}
  void submit(std::unique_ptr<gx::Frame> frame) override;
  void wait_for_room() override;
  std::unique_ptr<gx::Frame> recycle() override;
  // Waits up to `timeout_ms` for a frame; returns nullptr if none came.
  std::unique_ptr<gx::Frame> pop(int timeout_ms);
  // Hands a rendered frame back for reuse.
  void give_back(std::unique_ptr<gx::Frame> frame);

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<gx::Frame>> frames_;
  std::vector<std::unique_ptr<gx::Frame>> free_;
  std::size_t max_pending_;
};

// Something drawn over the presented picture (the menu).
class Overlay {
 public:
  virtual ~Overlay() = default;
  // Before the window's render pass (uploads).
  virtual void prepare(SDL_GPUCommandBuffer* cmd) = 0;
  // In the window's render pass, over the whole window.
  virtual void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) = 0;
};

// A shader as SPIR-V (Vulkan) and, if built with it, the Metal Shading Language.
struct ShaderCode {
  const u32* spirv;
  std::size_t spirv_size;
  const unsigned char* msl;
  std::size_t msl_size;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  static constexpr u32 kMaxScale = 8;

  // `window` may be null: frames are then rendered off screen (for
  // screenshots). `scale` is the internal resolution (1 = native).
  bool init(SDL_Window* window, u32 scale, std::string* error);

  // Changes the internal resolution from the next frame on.
  void set_scale(u32 scale);
  u32 scale() const { return scale_; }
  // Waits for the display's refresh to present (else presents at once,
  // tearing if need be).
  void set_vsync(bool vsync);
  // Drawn over every presented frame (nullptr: nothing).
  void set_overlay(Overlay* overlay) { overlay_ = overlay; }

  // Presents the last frame again (with the overlay), for when the game has
  // shown nothing new for a while.
  void redraw();

  SDL_GPUDevice* device() const { return device_; }
  u32 window_format() const;  // the window's SDL_GPUTextureFormat

  // Renders a frame and presents its XFB. If `capture` is given and the
  // frame has an XFB, it receives the XFB image (at the internal resolution).
  void render(const gx::Frame& frame, Image* capture = nullptr);

  const char* driver() const;

 private:
  struct Texture {
    SDL_GPUTexture* texture = nullptr;
    u32 width = 0, height = 0;  // host size (EFB copies: scaled)
  };

  SDL_GPUGraphicsPipeline* tev_pipeline(gx::PipelineKey key);
  SDL_GPUGraphicsPipeline* clear_pipeline(bool color, bool alpha, bool depth);
  SDL_GPUSampler* sampler(u32 mode0, u32 mode1);
  SDL_GPUShader* shader(const ShaderCode& code, bool fragment, u32 samplers, u32 uniforms);
  bool resize_efb(u32 width);
  bool upload(SDL_GPUCommandBuffer* cmd, const gx::Frame& frame);
  void begin_efb(SDL_GPUCommandBuffer* cmd);
  void end_pass();
  void draw(const gx::Frame& frame, const gx::DrawCmd& d);
  void clear(const gx::ClearCmd& c);
  void copy(SDL_GPUCommandBuffer* cmd, const gx::CopyCmd& c);
  void present(SDL_GPUCommandBuffer* cmd, const Texture* xfb, float aspect);
  void capture(SDL_GPUCommandBuffer* cmd, const Texture& xfb);
  void finish_capture(Image* image);
  void read_efb(SDL_GPUCommandBuffer* cmd, std::shared_ptr<gx::EfbReadback> request);
  void poll_reads(bool wait_all);
  void grow_buffer(SDL_GPUBuffer*& buffer, u32& capacity, u32 needed, u32 usage);

  SDL_Window* window_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  bool msl_ = false;  // the device takes MSL, not SPIR-V (Metal)
  Overlay* overlay_ = nullptr;
  u32 last_xfb_ = 0;  // texture handle of the last frame presented
  float last_aspect_ = 4.0f / 3.0f;
  u32 scale_ = 1, requested_scale_ = 1;
  u32 efb_logical_width_ = 0;         // in EFB pixels (640, or wider)
  u32 efb_width_ = 0, efb_height_ = 0;  // in host pixels
  SDL_GPUTexture* efb_color_ = nullptr;
  SDL_GPUTexture* efb_depth_ = nullptr;
  bool efb_initialised_ = false;
  SDL_GPUTexture* white_ = nullptr;  // bound where no texture is

  SDL_GPUShader* tev_vs_ = nullptr;
  SDL_GPUShader* tev_fs_ = nullptr;
  SDL_GPUShader* tev_fs_early_ = nullptr;
  SDL_GPUShader* fullscreen_vs_ = nullptr;
  SDL_GPUShader* copy_fs_ = nullptr;
  SDL_GPUShader* color_fs_ = nullptr;
  SDL_GPUShader* blit_fs_ = nullptr;
  SDL_GPUShader* sample_fs_ = nullptr;
  SDL_GPUGraphicsPipeline* read_color_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* read_depth_pipeline_ = nullptr;
  SDL_GPUTexture* read_color_ = nullptr;  // EFB read back at native resolution
  SDL_GPUTexture* read_depth_ = nullptr;

  // EFB readbacks recorded in this frame, and ones submitted but not yet read.
  struct PendingRead {
    std::shared_ptr<gx::EfbReadback> request;
    SDL_GPUTransferBuffer* transfer;
    std::shared_ptr<SDL_GPUFence> fence;
  };
  std::vector<PendingRead> unfenced_reads_, pending_reads_;
  struct Capture {
    SDL_GPUTransferBuffer* transfer = nullptr;
    u32 width = 0, height = 0;
  } capture_;
  SDL_GPUGraphicsPipeline* copy_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* present_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* capture_pipeline_ = nullptr;
  SDL_GPUSampler* linear_ = nullptr;
  SDL_GPUSampler* nearest_ = nullptr;
  std::unordered_map<u32, SDL_GPUGraphicsPipeline*> tev_pipelines_;
  std::unordered_map<u32, SDL_GPUGraphicsPipeline*> clear_pipelines_;
  std::unordered_map<u64, SDL_GPUSampler*> samplers_;
  std::unordered_map<u32, Texture> textures_;

  SDL_GPUBuffer* vertex_buffer_ = nullptr;
  SDL_GPUBuffer* index_buffer_ = nullptr;
  u32 vertex_capacity_ = 0, index_capacity_ = 0;
  SDL_GPUTransferBuffer* transfer_ = nullptr;
  u32 transfer_capacity_ = 0;

  // Per-frame render state
  SDL_GPUCommandBuffer* cmd_ = nullptr;
  SDL_GPURenderPass* pass_ = nullptr;
  bool pass_is_efb_ = false;
  SDL_GPUGraphicsPipeline* bound_pipeline_ = nullptr;
  u32 pushed_uniforms_ = ~0u;
};

// Writes an RGBA8 image as a PNG file.
bool write_png(const std::string& path, const Image& image);

}  // namespace video
