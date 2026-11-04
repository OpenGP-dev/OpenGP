// SPDX-License-Identifier: GPL-3.0-or-later
//
// The renderer: draws gx::Frames with SDL3's GPU API (Vulkan, or Metal on
// macOS) and shows their external framebuffer in a window.
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

#include <atomic>
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
struct SDL_GPUTextureCreateInfo;

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
  u64 wait_for_room() override;
  std::unique_ptr<gx::Frame> recycle() override;
  // Waits up to `timeout_ms` for a frame; returns nullptr if none came.
  std::unique_ptr<gx::Frame> pop(int timeout_ms);
  // Hands a rendered frame back for reuse.
  void give_back(std::unique_ptr<gx::Frame> frame);
  // Frames waiting to be rendered.
  std::size_t pending() {
    std::lock_guard lock(mutex_);
    return frames_.size();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::unique_ptr<gx::Frame>> frames_;
  std::vector<std::unique_ptr<gx::Frame>> free_;
  std::size_t max_pending_;
  // frames_.size(), readable without the lock: only the guest thread adds
  // frames, so what it reads is never fewer than there are.
  std::atomic<std::size_t> pending_{0};
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

// What the renderer did, for profiling: everything since the last
// Renderer::take_stats(). Times in nanoseconds.
struct RenderStats {
  u64 render_ns = 0;    // in render() and redraw(), all told
  u64 upload_ns = 0;    // copying frames' vertices, indices and textures for the GPU
  u64 acquire_ns = 0;   // waiting for the window's next image (the display's refresh with vsync, or the GPU)
  u64 submit_ns = 0;    // handing the commands to the GPU
  u64 fence_ns = 0;     // waiting for the GPU to finish (readbacks the game waits for, screenshots, GPU timing)
  u64 gpu_ns = 0;       // the GPU's time for the frames, measured only with OPENGP_PROFILE_GPU_SYNC=1
  u64 pipeline_ns = 0;  // creating pipelines (which compiles their shaders)
  u64 upload_bytes = 0;
  u32 renders = 0, presents = 0;
  u32 draws = 0, pipeline_binds = 0, passes = 0;
  u32 pipelines = 0, samplers = 0, textures = 0;  // created
  u64 texture_bytes = 0;                          // ... the textures' size
  u32 efb_resizes = 0;
  u32 failures = 0;  // GPU objects that could not be created, uploads that failed
  std::vector<u32> new_pipelines;  // the pipelines created (gx::PipelineKey bits)
};

// What a captured frame (gx::FrameCapture) needs from the GPU, at the
// internal resolution. Depth images hold 24-bit EFB depths in their red,
// green and blue bytes (red highest; alpha 255).
struct CaptureImages {
  Image efb_start, depth_start;  // the EFB before the frame's commands (empty for the first frame)
  Image efb_end, depth_end;      // the EFB just before its copy to the XFB
  Image xfb;                     // the picture shown
  struct Copy {
    u32 command;  // gx::Frame::commands
    Image image;
  };
  std::vector<Copy> copies;  // each EFB copy's result
  struct Earlier {
    u32 handle;
    Image image;
  };
  std::vector<Earlier> earlier;  // EFB copies earlier frames made that the frame samples, as it found them
};

// A small picture of a presented frame (see Renderer::request_thumbnail).
struct Thumbnail {
  u64 frame = 0;  // gx::Frame::number
  Image image;
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

  // Presents the last frame (with the overlay): again, for when the game has
  // shown nothing new for a while, or for the first time after a render()
  // that didn't. With `at_ns` (steady clock), it gets the window's next image,
  // then waits until then to present it; it returns how long it waited (ns).
  u64 redraw(u64 at_ns = 0);

  SDL_GPUDevice* device() const { return device_; }
  u32 window_format() const;  // the window's SDL_GPUTextureFormat

  // Renders a frame and presents its XFB (unless `present` is false: then
  // redraw() presents it). If `capture` is given and the frame has an XFB, it
  // receives the XFB image (at the internal resolution). `images` receives
  // what a frame capture needs (waiting for the GPU).
  void render(const gx::Frame& frame, Image* capture = nullptr, CaptureImages* images = nullptr, bool present = true);

  const char* driver() const;
  // The GPU's name and its driver's version, if the API says ("" if not).
  std::string device_name() const;
  std::string driver_version() const;

  // Profiling: what the renderer has done since the last call.
  RenderStats take_stats();
  u32 live_textures() const { return static_cast<u32>(textures_.size()); }
  u32 live_pipelines() const { return static_cast<u32>(tev_pipelines_.size() + clear_pipelines_.size()); }
  // Asks for a picture of the next frame with an XFB, `width` pixels wide at
  // most. It is copied without waiting for the GPU: take_thumbnails() returns
  // it a frame or two later.
  void request_thumbnail(u32 width) { thumbnail_width_ = width; }
  std::vector<Thumbnail> take_thumbnails();

 private:
  struct Texture {
    SDL_GPUTexture* texture = nullptr;
    u32 width = 0, height = 0;  // host size (EFB copies: scaled)
    u32 levels = 1;
    bool uploaded = false;  // a decoded texture (not an EFB copy target)
  };

  SDL_GPUGraphicsPipeline* tev_pipeline(gx::PipelineKey key);
  SDL_GPUGraphicsPipeline* clear_pipeline(bool color, bool alpha, bool depth);
  SDL_GPUSampler* sampler(u32 mode0, u32 mode1, bool native_lod);
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
  // EFB peeks follow the design of Dolphin's FramebufferManager and
  // EFBInterface: depth is scaled by 2^24 and clamped to 24 bits.
  void read_efb(SDL_GPUCommandBuffer* cmd, std::shared_ptr<gx::EfbReadback> request);
  u32 failed_frames_ = 0;  // frames in a row that could not be drawn
  void poll_reads(bool wait_all);
  void thumbnail(SDL_GPUCommandBuffer* cmd, const Texture& xfb, u64 frame);
  enum class Download { EfbStart, DepthStart, EfbEnd, DepthEnd, Xfb, Copy, Earlier };
  void download(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, u32 width, u32 height, Download kind, u32 id);
  void download_depth(SDL_GPUCommandBuffer* cmd, Download kind);
  void finish_downloads(CaptureImages* images);
  void poll_thumbnails(bool wait_all);
  SDL_GPUTexture* create_texture(const SDL_GPUTextureCreateInfo& info);
  bool grow_buffer(SDL_GPUBuffer*& buffer, u32& capacity, u32 needed, u32 usage);
  void abandon(const gx::Frame& f, const char* why);
  // New textures of frames that could not be drawn, made with the next one
  // that can (offsets into carried_texels_).
  std::vector<gx::TextureUpload> carried_uploads_;
  std::vector<u8> carried_texels_;

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
  std::vector<SDL_GPUTransferBuffer*> read_buffers_;  // download buffers for the next readbacks
  struct Capture {
    SDL_GPUTransferBuffer* transfer = nullptr;
    u32 width = 0, height = 0;
  } capture_;
  // Thumbnails being copied (see request_thumbnail()), and ready ones.
  struct PendingThumbnail {
    u64 frame;
    SDL_GPUTransferBuffer* transfer;
    u32 width, height;
    std::shared_ptr<SDL_GPUFence> fence;
  };
  u32 thumbnail_width_ = 0;
  std::vector<PendingThumbnail> unfenced_thumbnails_, pending_thumbnails_;
  std::vector<Thumbnail> thumbnails_;
  RenderStats stats_;
  // A frame capture's downloads (see CaptureImages), read once the GPU is done.
  struct PendingDownload {
    Download kind;
    u32 id;
    SDL_GPUTransferBuffer* transfer;
    u32 width, height;
  };
  std::vector<PendingDownload> downloads_;
  SDL_GPUTexture* depth_copy_ = nullptr;  // the EFB's depth as R32F, for downloading
  u32 depth_copy_width_ = 0, depth_copy_height_ = 0;
  bool gpu_sync_ = false;  // OPENGP_PROFILE_GPU_SYNC: wait for each frame, to time the GPU
  SDL_GPUGraphicsPipeline* copy_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* present_pipeline_ = nullptr;
  SDL_GPUGraphicsPipeline* capture_pipeline_ = nullptr;
  SDL_GPUSampler* linear_ = nullptr;
  SDL_GPUSampler* nearest_ = nullptr;
  std::unordered_map<u32, SDL_GPUGraphicsPipeline*> tev_pipelines_;
  std::unordered_map<u32, SDL_GPUGraphicsPipeline*> clear_pipelines_;
  std::unordered_map<u64, SDL_GPUSampler*> samplers_;
  std::unordered_map<u32, Texture> textures_;
  // Released decoded textures, kept for reuse by later textures of the same
  // size: creating one costs a command buffer submission in SDL's Vulkan
  // backend (about 75 us), and a course start creates dozens.
  struct PooledTexture {
    SDL_GPUTexture* texture;
    u32 width, height, levels;
    u64 released;  // frame
    u64 bytes;
  };
  std::deque<PooledTexture> texture_pool_;
  u64 texture_pool_bytes_ = 0;
  u64 frames_rendered_ = 0;
  SDL_GPUTexture* pooled_texture(u32 width, u32 height, u32 levels);
  void release_texture(const Texture& t);

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
