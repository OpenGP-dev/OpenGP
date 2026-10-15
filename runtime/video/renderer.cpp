// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "os/activity.h"

// SPIR-V, compiled from shaders/ at build time.
#include "blit_frag.h"
#include "color_frag.h"
#include "copy_frag.h"
#include "fullscreen_vert.h"
#include "sample_frag.h"
#include "tev_frag.h"
#include "tev_frag_early.h"
#include "tev_vert.h"

// The same shaders in the Metal Shading Language (see OPENGP_METAL_SHADERS).
#ifdef OPENGP_METAL_SHADERS
extern const unsigned char kTevVertMsl[];
extern const std::size_t kTevVertMsl_size;
extern const unsigned char kTevFragMsl[];
extern const std::size_t kTevFragMsl_size;
extern const unsigned char kTevFragEarlyMsl[];
extern const std::size_t kTevFragEarlyMsl_size;
extern const unsigned char kFullscreenVertMsl[];
extern const std::size_t kFullscreenVertMsl_size;
extern const unsigned char kCopyFragMsl[];
extern const std::size_t kCopyFragMsl_size;
extern const unsigned char kColorFragMsl[];
extern const std::size_t kColorFragMsl_size;
extern const unsigned char kBlitFragMsl[];
extern const std::size_t kBlitFragMsl_size;
extern const unsigned char kSampleFragMsl[];
extern const std::size_t kSampleFragMsl_size;
#define OPENGP_SHADER(name) ShaderCode{name, sizeof(name), name##Msl, name##Msl_size}
#else
#define OPENGP_SHADER(name) ShaderCode{name, sizeof(name), nullptr, 0}
#endif

namespace video {

namespace {

constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

// GX blend factors (see gx::PipelineKey) for the source and destination.
constexpr SDL_GPUBlendFactor kSrcFactor[8] = {
    SDL_GPU_BLENDFACTOR_ZERO,      SDL_GPU_BLENDFACTOR_ONE,       SDL_GPU_BLENDFACTOR_DST_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA};
constexpr SDL_GPUBlendFactor kDstFactor[8] = {
    SDL_GPU_BLENDFACTOR_ZERO,      SDL_GPU_BLENDFACTOR_ONE,       SDL_GPU_BLENDFACTOR_SRC_COLOR,
    SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA};

u32 field(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }

struct FullscreenUniforms {
  float rect[4];
  float z[4];
};

struct CopyUniforms {
  float pixel[4];
  u32 params[4];
  u32 filter[4];
};

}  // namespace

// ---- FrameQueue ------------------------------------------------------------------------------

void FrameQueue::submit(std::unique_ptr<gx::Frame> frame) {
  std::lock_guard lock(mutex_);
  frames_.push_back(std::move(frame));
  cv_.notify_all();
}

void FrameQueue::wait_for_room() {
  std::unique_lock lock(mutex_);
  if (frames_.size() < max_pending_) return;
  os::g_guest_wait.set("waiting for the renderer to take a frame");
  cv_.wait(lock, [this] { return frames_.size() < max_pending_; });
  os::g_guest_wait.set(nullptr);
}

std::unique_ptr<gx::Frame> FrameQueue::recycle() {
  std::lock_guard lock(mutex_);
  if (free_.empty()) return nullptr;
  auto frame = std::move(free_.back());
  free_.pop_back();
  return frame;
}

void FrameQueue::give_back(std::unique_ptr<gx::Frame> frame) {
  std::lock_guard lock(mutex_);
  if (free_.size() < 4) free_.push_back(std::move(frame));
}

std::unique_ptr<gx::Frame> FrameQueue::pop(int timeout_ms) {
  std::unique_lock lock(mutex_);
  if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !frames_.empty(); })) return nullptr;
  auto frame = std::move(frames_.front());
  frames_.pop_front();
  cv_.notify_all();
  return frame;
}

// ---- Setup -----------------------------------------------------------------------------------

Renderer::Renderer() = default;

Renderer::~Renderer() {
  if (!device_) return;
  SDL_WaitForGPUIdle(device_);
  for (auto& [h, t] : textures_) SDL_ReleaseGPUTexture(device_, t.texture);
  for (PooledTexture& t : texture_pool_) SDL_ReleaseGPUTexture(device_, t.texture);
  for (auto& [k, p] : tev_pipelines_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
  for (auto& [k, p] : clear_pipelines_) SDL_ReleaseGPUGraphicsPipeline(device_, p);
  for (auto& [k, s] : samplers_) SDL_ReleaseGPUSampler(device_, s);
  for (SDL_GPUGraphicsPipeline* p : {copy_pipeline_, present_pipeline_, capture_pipeline_, read_color_pipeline_, read_depth_pipeline_})
    if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
  for (SDL_GPUShader* s : {tev_vs_, tev_fs_, tev_fs_early_, fullscreen_vs_, copy_fs_, color_fs_, blit_fs_, sample_fs_})
    if (s) SDL_ReleaseGPUShader(device_, s);
  for (SDL_GPUSampler* s : {linear_, nearest_})
    if (s) SDL_ReleaseGPUSampler(device_, s);
  for (SDL_GPUTexture* t : {efb_color_, efb_depth_, white_, read_color_, read_depth_})
    if (t) SDL_ReleaseGPUTexture(device_, t);
  if (vertex_buffer_) SDL_ReleaseGPUBuffer(device_, vertex_buffer_);
  if (index_buffer_) SDL_ReleaseGPUBuffer(device_, index_buffer_);
  if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
  poll_reads(true);
  for (SDL_GPUTransferBuffer* b : read_buffers_) SDL_ReleaseGPUTransferBuffer(device_, b);
  if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
  SDL_DestroyGPUDevice(device_);
}

const char* Renderer::driver() const { return device_ ? SDL_GetGPUDeviceDriver(device_) : "none"; }

SDL_GPUShader* Renderer::shader(const ShaderCode& code, bool fragment, u32 samplers, u32 uniforms) {
  SDL_GPUShaderCreateInfo info{};
  if (msl_) {
    info.code = code.msl;
    info.code_size = code.msl_size;
    info.entrypoint = "main0";  // SPIRV-Cross's name for main
    info.format = SDL_GPU_SHADERFORMAT_MSL;
  } else {
    info.code = reinterpret_cast<const Uint8*>(code.spirv);
    info.code_size = code.spirv_size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
  }
  info.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
  info.num_samplers = samplers;
  info.num_uniform_buffers = uniforms;
  return SDL_CreateGPUShader(device_, &info);
}

bool Renderer::init(SDL_Window* window, u32 scale, std::string* error) {
  const char* debug_env = std::getenv("OPENGP_GPU_DEBUG");
  const bool debug = debug_env && *debug_env && *debug_env != '0';
  // Vulkan (SPIR-V), or Metal (MSL) on macOS: SDL picks the first backend
  // that takes one of the formats (Metal before Vulkan).
  SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef OPENGP_METAL_SHADERS
  formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
  device_ = SDL_CreateGPUDevice(formats, debug, nullptr);
  if (!device_) {
    *error = std::string("could not create a GPU device (Vulkan or Metal is required): ") + SDL_GetError();
    return false;
  }
  msl_ = !(SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_SPIRV);
  if (window) {
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
      *error = std::string("could not use the window for rendering: ") + SDL_GetError();
      return false;
    }
    window_ = window;
    SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
  }
  set_scale(scale);
  if (!resize_efb(gx::kEfbWidth)) {
    *error = std::string("could not create the EFB: ") + SDL_GetError();
    return false;
  }
  SDL_GPUTextureCreateInfo t{};
  t.type = SDL_GPU_TEXTURETYPE_2D;
  t.format = kColorFormat;
  t.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  t.width = t.height = 1;
  t.layer_count_or_depth = 1;
  t.num_levels = 1;
  white_ = SDL_CreateGPUTexture(device_, &t);
  if (!white_) {
    *error = std::string("could not create a texture: ") + SDL_GetError();
    return false;
  }

  tev_vs_ = shader(OPENGP_SHADER(kTevVert), false, 0, 0);
  tev_fs_ = shader(OPENGP_SHADER(kTevFrag), true, 8, 2);
  tev_fs_early_ = shader(OPENGP_SHADER(kTevFragEarly), true, 8, 2);
  fullscreen_vs_ = shader(OPENGP_SHADER(kFullscreenVert), false, 0, 1);
  copy_fs_ = shader(OPENGP_SHADER(kCopyFrag), true, 1, 1);
  color_fs_ = shader(OPENGP_SHADER(kColorFrag), true, 0, 1);
  blit_fs_ = shader(OPENGP_SHADER(kBlitFrag), true, 1, 0);
  sample_fs_ = shader(OPENGP_SHADER(kSampleFrag), true, 1, 0);
  if (!tev_vs_ || !tev_fs_ || !tev_fs_early_ || !fullscreen_vs_ || !copy_fs_ || !color_fs_ || !blit_fs_ || !sample_fs_) {
    *error = std::string("could not create shaders: ") + SDL_GetError();
    return false;
  }

  SDL_GPUSamplerCreateInfo s{};
  s.min_filter = s.mag_filter = SDL_GPU_FILTER_LINEAR;
  s.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  s.address_mode_u = s.address_mode_v = s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  linear_ = SDL_CreateGPUSampler(device_, &s);
  s.min_filter = s.mag_filter = SDL_GPU_FILTER_NEAREST;
  nearest_ = SDL_CreateGPUSampler(device_, &s);

  // Fullscreen pipelines (copies into RGBA8 textures, presenting).
  auto fullscreen_pipeline = [&](SDL_GPUShader* fs, SDL_GPUTextureFormat format) {
    SDL_GPUColorTargetDescription target{};
    target.format = format;
    SDL_GPUGraphicsPipelineCreateInfo p{};
    p.vertex_shader = fullscreen_vs_;
    p.fragment_shader = fs;
    p.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    p.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    p.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    p.target_info.color_target_descriptions = &target;
    p.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(device_, &p);
  };
  copy_pipeline_ = fullscreen_pipeline(copy_fs_, kColorFormat);
  capture_pipeline_ = fullscreen_pipeline(blit_fs_, kColorFormat);
  if (window_) present_pipeline_ = fullscreen_pipeline(blit_fs_, SDL_GetGPUSwapchainTextureFormat(device_, window_));
  read_color_pipeline_ = fullscreen_pipeline(sample_fs_, kColorFormat);
  read_depth_pipeline_ = fullscreen_pipeline(sample_fs_, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
  if (!copy_pipeline_ || !capture_pipeline_ || (window_ && !present_pipeline_) || !read_color_pipeline_ ||
      !read_depth_pipeline_) {
    *error = std::string("could not create pipelines: ") + SDL_GetError();
    return false;
  }

  // The white texture: upload one pixel.
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
  SDL_GPUTransferBufferCreateInfo tb{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 4, 0};
  SDL_GPUTransferBuffer* one = SDL_CreateGPUTransferBuffer(device_, &tb);
  std::memset(SDL_MapGPUTransferBuffer(device_, one, false), 0xFF, 4);
  SDL_UnmapGPUTransferBuffer(device_, one);
  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureTransferInfo src{one, 0, 1, 1};
  SDL_GPUTextureRegion dst{};
  dst.texture = white_;
  dst.w = dst.h = dst.d = 1;
  SDL_UploadToGPUTexture(copy, &src, &dst, false);
  SDL_EndGPUCopyPass(copy);
  SDL_SubmitGPUCommandBuffer(cmd);
  SDL_ReleaseGPUTransferBuffer(device_, one);
  return true;
}

void Renderer::set_scale(u32 scale) { requested_scale_ = std::clamp(scale, 1u, kMaxScale); }

void Renderer::set_vsync(bool vsync) {
  if (!window_) return;
  SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
  if (!vsync) {
    mode = SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX) ? SDL_GPU_PRESENTMODE_MAILBOX
           : SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE)
               ? SDL_GPU_PRESENTMODE_IMMEDIATE
               : SDL_GPU_PRESENTMODE_VSYNC;
  }
  SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
}

u32 Renderer::window_format() const {
  return window_ ? static_cast<u32>(SDL_GetGPUSwapchainTextureFormat(device_, window_)) : SDL_GPU_TEXTUREFORMAT_INVALID;
}

bool Renderer::resize_efb(u32 width) {
  // The EFB is (re)created at the internal resolution and the frame's width;
  // its contents are lost (the next frame draws them again).
  const u32 scale = requested_scale_;
  if (efb_color_ && efb_depth_ && width == efb_logical_width_ && scale == scale_) return true;
  end_pass();
  if (efb_color_) SDL_ReleaseGPUTexture(device_, efb_color_);
  if (efb_depth_) SDL_ReleaseGPUTexture(device_, efb_depth_);
  scale_ = scale;
  efb_logical_width_ = width;
  efb_width_ = width * scale_;
  efb_height_ = gx::kEfbHeight * scale_;
  SDL_GPUTextureCreateInfo t{};
  t.type = SDL_GPU_TEXTURETYPE_2D;
  t.format = kColorFormat;
  t.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  t.width = efb_width_;
  t.height = efb_height_;
  t.layer_count_or_depth = 1;
  t.num_levels = 1;
  efb_color_ = SDL_CreateGPUTexture(device_, &t);
  t.format = kDepthFormat;
  t.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  efb_depth_ = SDL_CreateGPUTexture(device_, &t);
  efb_initialised_ = false;
  return efb_color_ && efb_depth_;
}

// ---- Pipelines and samplers ------------------------------------------------------------------

SDL_GPUGraphicsPipeline* Renderer::tev_pipeline(gx::PipelineKey key) {
  if (auto it = tev_pipelines_.find(key.bits); it != tev_pipelines_.end()) return it->second;
  const u32 k = key.bits;

  SDL_GPUVertexBufferDescription vb{0, sizeof(gx::Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
  SDL_GPUVertexAttribute attrs[11];
  attrs[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(gx::Vertex, pos)};
  attrs[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(gx::Vertex, color)};
  attrs[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(gx::Vertex, color) + 4};
  for (u32 i = 0; i < 8; ++i)
    attrs[3 + i] = {3 + i, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, static_cast<u32>(offsetof(gx::Vertex, tex) + 12 * i)};

  SDL_GPUColorTargetDescription target{};
  target.format = kColorFormat;
  SDL_GPUColorTargetBlendState& b = target.blend_state;
  b.enable_blend = field(k, 0, 1);
  b.src_color_blendfactor = kSrcFactor[field(k, 1, 3)];
  b.dst_color_blendfactor = kDstFactor[field(k, 4, 3)];
  b.src_alpha_blendfactor = kSrcFactor[field(k, 7, 3)];
  b.dst_alpha_blendfactor = kDstFactor[field(k, 10, 3)];
  b.color_blend_op = field(k, 13, 1) ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
  b.alpha_blend_op = field(k, 14, 1) ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
  b.enable_color_write_mask = true;
  b.color_write_mask = (field(k, 15, 1) ? (SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B) : 0) |
                       (field(k, 16, 1) ? SDL_GPU_COLORCOMPONENT_A : 0);

  SDL_GPUGraphicsPipelineCreateInfo p{};
  p.vertex_shader = tev_vs_;
  p.fragment_shader = field(k, 22, 1) ? tev_fs_early_ : tev_fs_;
  p.vertex_input_state = {&vb, 1, attrs, 11};
  p.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  p.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  p.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // culled on the CPU
  p.rasterizer_state.enable_depth_clip = true;
  p.depth_stencil_state.enable_depth_test = field(k, 17, 1);
  p.depth_stencil_state.compare_op = static_cast<SDL_GPUCompareOp>(SDL_GPU_COMPAREOP_NEVER + field(k, 18, 3));
  p.depth_stencil_state.enable_depth_write = field(k, 17, 1) && field(k, 21, 1);
  p.target_info.color_target_descriptions = &target;
  p.target_info.num_color_targets = 1;
  p.target_info.depth_stencil_format = kDepthFormat;
  p.target_info.has_depth_stencil_target = true;
  SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &p);
  if (!pipeline) std::fprintf(stderr, "[video] pipeline %08X: %s\n", k, SDL_GetError());
  tev_pipelines_.emplace(key.bits, pipeline);
  return pipeline;
}

SDL_GPUGraphicsPipeline* Renderer::clear_pipeline(bool color, bool alpha, bool depth) {
  const u32 key = u32{color} | (u32{alpha} << 1) | (u32{depth} << 2);
  if (auto it = clear_pipelines_.find(key); it != clear_pipelines_.end()) return it->second;
  SDL_GPUColorTargetDescription target{};
  target.format = kColorFormat;
  target.blend_state.enable_color_write_mask = true;
  target.blend_state.color_write_mask =
      (color ? (SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B) : 0) |
      (alpha ? SDL_GPU_COLORCOMPONENT_A : 0);
  SDL_GPUGraphicsPipelineCreateInfo p{};
  p.vertex_shader = fullscreen_vs_;
  p.fragment_shader = color_fs_;
  p.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  p.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  p.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
  p.depth_stencil_state.enable_depth_test = depth;
  p.depth_stencil_state.enable_depth_write = depth;
  p.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
  p.target_info.color_target_descriptions = &target;
  p.target_info.num_color_targets = 1;
  p.target_info.depth_stencil_format = kDepthFormat;
  p.target_info.has_depth_stencil_target = true;
  SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &p);
  clear_pipelines_.emplace(key, pipeline);
  return pipeline;
}

SDL_GPUSampler* Renderer::sampler(u32 mode0, u32 mode1) {
  const u64 key = (u64{mode0 & 0x1FFFFF} << 16) | (mode1 & 0xFFFF);
  if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
  auto wrap = [](u32 w) {
    return w == 0 ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
                  : w == 2 ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  };
  const u32 mip = field(mode0, 5, 2);
  SDL_GPUSamplerCreateInfo s{};
  s.address_mode_u = wrap(field(mode0, 0, 2));
  s.address_mode_v = wrap(field(mode0, 2, 2));
  s.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  s.mag_filter = field(mode0, 4, 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
  s.min_filter = field(mode0, 7, 1) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
  s.mipmap_mode = mip == 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
  s.mip_lod_bias = static_cast<float>(static_cast<s8>(field(mode0, 9, 8))) / 32.0f;
  if (mip == 0) {
    s.min_lod = s.max_lod = 0.0f;
  } else {
    s.min_lod = static_cast<float>(field(mode1, 0, 8)) / 16.0f;
    s.max_lod = static_cast<float>(field(mode1, 8, 8)) / 16.0f;
  }
  SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &s);
  samplers_.emplace(key, sampler);
  return sampler;
}

// ---- Rendering -------------------------------------------------------------------------------

void Renderer::grow_buffer(SDL_GPUBuffer*& buffer, u32& capacity, u32 needed, u32 usage) {
  if (needed <= capacity) return;
  if (buffer) SDL_ReleaseGPUBuffer(device_, buffer);
  capacity = std::max(needed, capacity * 2);
  SDL_GPUBufferCreateInfo info{usage, capacity, 0};
  buffer = SDL_CreateGPUBuffer(device_, &info);
}

bool Renderer::upload(SDL_GPUCommandBuffer* cmd, const gx::Frame& f) {
  const u32 vbytes = static_cast<u32>(f.vertices.size() * sizeof(gx::Vertex));
  const u32 ibytes = static_cast<u32>(f.indices.size() * sizeof(u32));
  const u32 tbytes = static_cast<u32>(f.texels.size());
  const u32 total = vbytes + ibytes + tbytes;
  if (total == 0) return true;

  if (total > transfer_capacity_) {
    if (transfer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_);
  poll_reads(true);
    transfer_capacity_ = std::max(total, transfer_capacity_ * 2);
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, transfer_capacity_, 0};
    transfer_ = SDL_CreateGPUTransferBuffer(device_, &info);
    if (!transfer_) return false;
  }
  if (vbytes) grow_buffer(vertex_buffer_, vertex_capacity_, vbytes, SDL_GPU_BUFFERUSAGE_VERTEX);
  if (ibytes) grow_buffer(index_buffer_, index_capacity_, ibytes, SDL_GPU_BUFFERUSAGE_INDEX);

  auto* dst = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, transfer_, true));
  if (!dst) return false;
  std::memcpy(dst, f.vertices.data(), vbytes);
  std::memcpy(dst + vbytes, f.indices.data(), ibytes);
  std::memcpy(dst + vbytes + ibytes, f.texels.data(), tbytes);
  SDL_UnmapGPUTransferBuffer(device_, transfer_);

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  if (vbytes) {
    SDL_GPUTransferBufferLocation src{transfer_, 0};
    SDL_GPUBufferRegion region{vertex_buffer_, 0, vbytes};
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
  }
  if (ibytes) {
    SDL_GPUTransferBufferLocation src{transfer_, vbytes};
    SDL_GPUBufferRegion region{index_buffer_, 0, ibytes};
    SDL_UploadToGPUBuffer(copy, &src, &region, true);
  }
  for (const gx::TextureUpload& up : f.uploads) {
    SDL_GPUTexture* tex = pooled_texture(up.width, up.height, up.levels);
    const bool reused = tex != nullptr;
    if (!tex) {
      SDL_GPUTextureCreateInfo t{};
      t.type = SDL_GPU_TEXTURETYPE_2D;
      t.format = kColorFormat;
      t.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      t.width = up.width;
      t.height = up.height;
      t.layer_count_or_depth = 1;
      t.num_levels = up.levels;
      tex = SDL_CreateGPUTexture(device_, &t);
    }
    if (!tex) continue;
    u32 offset = vbytes + ibytes + static_cast<u32>(up.offset);
    for (u32 l = 0, w = up.width, h = up.height; l < up.levels; ++l) {
      SDL_GPUTextureTransferInfo src{transfer_, offset, w, h};
      SDL_GPUTextureRegion region{};
      region.texture = tex;
      region.mip_level = l;
      region.w = w;
      region.h = h;
      region.d = 1;
      // A reused texture gets new memory if the GPU might still be reading
      // it (cycle, with the first level only: cycling replaces all of them).
      SDL_UploadToGPUTexture(copy, &src, &region, reused && l == 0);
      offset += w * h * 4;
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    textures_[up.handle] = {tex, up.width, up.height, up.levels, true};
  }
  SDL_EndGPUCopyPass(copy);
  return true;
}

void Renderer::begin_efb(SDL_GPUCommandBuffer* cmd) {
  if (pass_ && pass_is_efb_) return;
  end_pass();
  SDL_GPUColorTargetInfo color{};
  color.texture = efb_color_;
  color.load_op = efb_initialised_ ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
  color.clear_color = {0, 0, 0, 1};
  color.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPUDepthStencilTargetInfo depth{};
  depth.texture = efb_depth_;
  depth.load_op = efb_initialised_ ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
  depth.clear_depth = 1.0f;
  depth.store_op = SDL_GPU_STOREOP_STORE;
  depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
  depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
  efb_initialised_ = true;
  pass_ = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
  pass_is_efb_ = true;
  bound_pipeline_ = nullptr;
  pushed_uniforms_ = ~0u;
  SDL_GPUViewport vp{0, 0, static_cast<float>(efb_width_), static_cast<float>(efb_height_), 0.0f, 1.0f};
  SDL_SetGPUViewport(pass_, &vp);
  if (vertex_buffer_) {
    SDL_GPUBufferBinding vb{vertex_buffer_, 0};
    SDL_BindGPUVertexBuffers(pass_, 0, &vb, 1);
  }
  if (index_buffer_) {
    SDL_GPUBufferBinding ib{index_buffer_, 0};
    SDL_BindGPUIndexBuffer(pass_, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
  }
  const float host[4] = {static_cast<float>(scale_), static_cast<float>((efb_logical_width_ - gx::kEfbWidth) / 2), 0, 0};
  SDL_PushGPUFragmentUniformData(cmd, 1, host, sizeof(host));
}

void Renderer::end_pass() {
  if (pass_) SDL_EndGPURenderPass(pass_);
  pass_ = nullptr;
  pass_is_efb_ = false;
}

void Renderer::draw(const gx::Frame& f, const gx::DrawCmd& d) {
  const gx::DrawState& s = f.states[d.state];
  SDL_GPUGraphicsPipeline* pipeline = tev_pipeline(s.pipeline);
  if (!pipeline) return;
  if (pipeline != bound_pipeline_) {
    SDL_BindGPUGraphicsPipeline(pass_, pipeline);
    bound_pipeline_ = pipeline;
  }
  SDL_Rect scissor{s.scissor[0] * static_cast<int>(scale_), s.scissor[1] * static_cast<int>(scale_),
                   s.scissor[2] * static_cast<int>(scale_), s.scissor[3] * static_cast<int>(scale_)};
  SDL_SetGPUScissor(pass_, &scissor);

  SDL_GPUTextureSamplerBinding bindings[8];
  for (u32 i = 0; i < 8; ++i) {
    const gx::TextureBinding& t = s.textures[i];
    auto it = t.texture ? textures_.find(t.texture) : textures_.end();
    bindings[i].texture = it != textures_.end() ? it->second.texture : white_;
    bindings[i].sampler = sampler(t.mode0, t.mode1);
  }
  SDL_BindGPUFragmentSamplers(pass_, 0, bindings, 8);
  if (s.uniforms != pushed_uniforms_) {
    SDL_PushGPUFragmentUniformData(cmd_, 0, &f.uniforms[s.uniforms],
                                   sizeof(gx::PixelUniforms));
    pushed_uniforms_ = s.uniforms;
  }
  SDL_DrawGPUIndexedPrimitives(pass_, d.index_count, 1, d.first_index, 0, 0);
}

void Renderer::clear(const gx::ClearCmd& c) {
  SDL_GPUGraphicsPipeline* pipeline = clear_pipeline(c.color, c.alpha, c.depth);
  if (!pipeline) return;
  SDL_BindGPUGraphicsPipeline(pass_, pipeline);
  bound_pipeline_ = pipeline;
  SDL_Rect scissor{static_cast<int>(c.x * scale_), static_cast<int>(c.y * scale_), static_cast<int>(c.width * scale_),
                   static_cast<int>(c.height * scale_)};
  SDL_SetGPUScissor(pass_, &scissor);
  SDL_GPUCommandBuffer* cmd = cmd_;
  const FullscreenUniforms vs{{0, 0, 1, 1}, {c.z, 0, 0, 0}};
  SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
  const float color[4] = {static_cast<float>(c.rgba & 0xFF) / 255.0f, static_cast<float>((c.rgba >> 8) & 0xFF) / 255.0f,
                          static_cast<float>((c.rgba >> 16) & 0xFF) / 255.0f, static_cast<float>(c.rgba >> 24) / 255.0f};
  SDL_PushGPUFragmentUniformData(cmd, 0, color, sizeof(color));
  pushed_uniforms_ = ~0u;
  SDL_DrawGPUPrimitives(pass_, 3, 1, 0, 0);
}

void Renderer::copy(SDL_GPUCommandBuffer* cmd, const gx::CopyCmd& c) {
  end_pass();
  const u32 w = c.width * scale_, h = c.height * scale_;
  Texture& dst = textures_[c.dest];
  if (!dst.texture || dst.width != w || dst.height != h) {
    if (dst.texture) SDL_ReleaseGPUTexture(device_, dst.texture);
    SDL_GPUTextureCreateInfo t{};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = kColorFormat;
    t.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    t.width = w;
    t.height = h;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    dst = {SDL_CreateGPUTexture(device_, &t), w, h};
    if (!dst.texture) return;
  }

  SDL_GPUColorTargetInfo target{};
  target.texture = dst.texture;
  target.load_op = SDL_GPU_LOADOP_DONT_CARE;
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, copy_pipeline_);
  SDL_GPUTextureSamplerBinding src{c.depth ? efb_depth_ : efb_color_, c.depth ? nearest_ : linear_};
  SDL_BindGPUFragmentSamplers(pass, 0, &src, 1);

  const float ew = static_cast<float>(efb_logical_width_), eh = gx::kEfbHeight;
  const FullscreenUniforms vs{{c.src_x / ew, c.src_y / eh, (c.src_x + c.src_width) / ew, (c.src_y + c.src_height) / eh},
                              {0, 0, 0, 0}};
  SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
  CopyUniforms fs{};
  fs.pixel[0] = 1.0f / static_cast<float>(efb_height_);
  fs.pixel[1] = c.clamp_top ? (c.src_y + 0.5f) / eh : 0.0f;
  fs.pixel[2] = c.clamp_bottom ? (c.src_y + c.src_height - 0.5f) / eh : 1.0f;
  fs.pixel[3] = 1.0f / c.gamma;
  fs.params[0] = static_cast<u32>(c.format);
  fs.params[1] = (c.depth ? 1u : 0u) | (c.intensity ? 2u : 0u) | (c.efb_has_alpha ? 4u : 0u) | (c.gamma != 1.0f ? 8u : 0u);
  fs.filter[0] = c.filter[0];
  fs.filter[1] = c.filter[1];
  fs.filter[2] = c.filter[2];
  SDL_PushGPUFragmentUniformData(cmd, 0, &fs, sizeof(fs));
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

void Renderer::present(SDL_GPUCommandBuffer* cmd, const Texture* xfb, float aspect) {
  SDL_GPUTexture* swapchain = nullptr;
  Uint32 sw = 0, sh = 0;
  os::g_main_activity.set("waiting for the window's next image");
  const bool acquired = SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &sw, &sh);
  os::g_main_activity.set("drawing a frame");
  if (!acquired || !swapchain) return;
  if (overlay_) overlay_->prepare(cmd);
  SDL_GPUColorTargetInfo target{};
  target.texture = swapchain;
  target.load_op = SDL_GPU_LOADOP_CLEAR;
  target.clear_color = {0, 0, 0, 1};
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  if (xfb) {
    // Letterbox to the XFB's aspect ratio.
    float w = static_cast<float>(sw), h = static_cast<float>(sh);
    if (w > h * aspect) w = h * aspect;
    else h = w / aspect;
    SDL_GPUViewport vp{(static_cast<float>(sw) - w) / 2, (static_cast<float>(sh) - h) / 2, w, h, 0, 1};
    SDL_SetGPUViewport(pass, &vp);
    SDL_BindGPUGraphicsPipeline(pass, present_pipeline_);
    SDL_GPUTextureSamplerBinding src{xfb->texture, linear_};
    SDL_BindGPUFragmentSamplers(pass, 0, &src, 1);
    const FullscreenUniforms vs{{0, 0, 1, 1}, {0, 0, 0, 0}};
    SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_GPUViewport all{0, 0, static_cast<float>(sw), static_cast<float>(sh), 0, 1};
    SDL_SetGPUViewport(pass, &all);
  }
  if (overlay_) overlay_->draw(cmd, pass);
  SDL_EndGPURenderPass(pass);
}

void Renderer::redraw() {
  if (!window_) return;
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
  if (!cmd) return;
  const auto xfb = last_xfb_ ? textures_.find(last_xfb_) : textures_.end();
  present(cmd, xfb != textures_.end() && xfb->second.texture ? &xfb->second : nullptr, last_aspect_);
  SDL_SubmitGPUCommandBuffer(cmd);
}

void Renderer::capture(SDL_GPUCommandBuffer* cmd, const Texture& xfb) {
  // Copy the XFB (alpha forced to 1) into a download buffer; finish_capture()
  // reads it once the command buffer has run.
  SDL_GPUTextureCreateInfo t{};
  t.type = SDL_GPU_TEXTURETYPE_2D;
  t.format = kColorFormat;
  t.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
  t.width = xfb.width;
  t.height = xfb.height;
  t.layer_count_or_depth = 1;
  t.num_levels = 1;
  SDL_GPUTexture* opaque = SDL_CreateGPUTexture(device_, &t);
  SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, xfb.width * xfb.height * 4, 0};
  SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(device_, &info);
  if (!opaque || !download) {
    if (opaque) SDL_ReleaseGPUTexture(device_, opaque);
    if (download) SDL_ReleaseGPUTransferBuffer(device_, download);
    return;
  }

  SDL_GPUColorTargetInfo target{};
  target.texture = opaque;
  target.load_op = SDL_GPU_LOADOP_DONT_CARE;
  target.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, capture_pipeline_);
  SDL_GPUTextureSamplerBinding src{xfb.texture, nearest_};
  SDL_BindGPUFragmentSamplers(pass, 0, &src, 1);
  const FullscreenUniforms vs{{0, 0, 1, 1}, {0, 0, 0, 0}};
  SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion region{};
  region.texture = opaque;
  region.w = xfb.width;
  region.h = xfb.height;
  region.d = 1;
  SDL_GPUTextureTransferInfo dst{download, 0, xfb.width, xfb.height};
  SDL_DownloadFromGPUTexture(copy, &region, &dst);
  SDL_EndGPUCopyPass(copy);
  SDL_ReleaseGPUTexture(device_, opaque);  // freed once the command buffer is done with it
  capture_ = {download, xfb.width, xfb.height};
}

void Renderer::finish_capture(Image* image) {
  if (!capture_.transfer) return;
  image->width = capture_.width;
  image->height = capture_.height;
  image->rgba.resize(std::size_t{capture_.width} * capture_.height * 4);
  if (const void* p = SDL_MapGPUTransferBuffer(device_, capture_.transfer, false)) {
    std::memcpy(image->rgba.data(), p, image->rgba.size());
    SDL_UnmapGPUTransferBuffer(device_, capture_.transfer);
  }
  SDL_ReleaseGPUTransferBuffer(device_, capture_.transfer);
  capture_ = {};
}

void Renderer::read_efb(SDL_GPUCommandBuffer* cmd, std::shared_ptr<gx::EfbReadback> request) {
  // Sample the EFB at native resolution into a colour target (depth goes to
  // .r of an R32F target), then download it. The request completes once the
  // command buffer has run (poll_reads()).
  const u32 w = gx::kEfbWidth, h = gx::kEfbHeight;
  SDL_GPUTexture*& target = request->depth ? read_depth_ : read_color_;
  if (!target) {
    SDL_GPUTextureCreateInfo t{};
    t.type = SDL_GPU_TEXTURETYPE_2D;
    t.format = request->depth ? SDL_GPU_TEXTUREFORMAT_R32_FLOAT : kColorFormat;
    t.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    t.width = w;
    t.height = h;
    t.layer_count_or_depth = 1;
    t.num_levels = 1;
    target = SDL_CreateGPUTexture(device_, &t);
  }
  // Download buffers are kept for the next readbacks: creating one each time
  // (once a frame on some courses) sometimes stalled the driver for 4-40 ms.
  SDL_GPUTransferBuffer* transfer = nullptr;
  if (!read_buffers_.empty()) {
    transfer = read_buffers_.back();
    read_buffers_.pop_back();
  } else {
    SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, w * h * 4, 0};
    transfer = SDL_CreateGPUTransferBuffer(device_, &info);
  }
  if (!target || !transfer) {
    if (transfer) read_buffers_.push_back(transfer);
    request->complete();  // with no pixels: peeks read zero
    return;
  }

  end_pass();
  SDL_GPUColorTargetInfo color{};
  color.texture = target;
  color.load_op = SDL_GPU_LOADOP_DONT_CARE;
  color.store_op = SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
  SDL_BindGPUGraphicsPipeline(pass, request->depth ? read_depth_pipeline_ : read_color_pipeline_);
  SDL_GPUTextureSamplerBinding src{request->depth ? efb_depth_ : efb_color_, nearest_};
  SDL_BindGPUFragmentSamplers(pass, 0, &src, 1);
  // The game's part of a wide EFB.
  const float ew = static_cast<float>(efb_logical_width_), side = (ew - static_cast<float>(w)) / 2;
  const FullscreenUniforms vs{{side / ew, 0, (side + static_cast<float>(w)) / ew, 1}, {0, 0, 0, 0}};
  SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);

  SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
  SDL_GPUTextureRegion region{};
  region.texture = target;
  region.w = w;
  region.h = h;
  region.d = 1;
  SDL_GPUTextureTransferInfo dst{transfer, 0, w, h};
  SDL_DownloadFromGPUTexture(copy, &region, &dst);
  SDL_EndGPUCopyPass(copy);
  unfenced_reads_.push_back({std::move(request), transfer, nullptr});
}

void Renderer::poll_reads(bool wait_all) {
  for (auto it = pending_reads_.begin(); it != pending_reads_.end();) {
    SDL_GPUFence* fence = it->fence.get();
    if (!wait_all && !SDL_QueryGPUFence(device_, fence)) {
      ++it;
      continue;
    }
    if (wait_all) {
      os::g_main_activity.set("waiting for the GPU (readbacks)");
      SDL_WaitForGPUFences(device_, true, &fence, 1);
      os::g_main_activity.set("drawing a frame");
    }
    gx::EfbReadback& r = *it->request;
    r.pixels.assign(std::size_t{gx::kEfbWidth} * gx::kEfbHeight, 0);
    if (const void* p = SDL_MapGPUTransferBuffer(device_, it->transfer, false)) {
      std::memcpy(r.pixels.data(), p, r.pixels.size() * 4);
      SDL_UnmapGPUTransferBuffer(device_, it->transfer);
      if (r.depth) {
        for (u32& v : r.pixels) {
          float d;
          std::memcpy(&d, &v, 4);
          v = static_cast<u32>(std::clamp(d * 16777216.0f, 0.0f, 16777215.0f));
        }
      }
    }
    if (read_buffers_.size() < 4) read_buffers_.push_back(it->transfer);
    else SDL_ReleaseGPUTransferBuffer(device_, it->transfer);
    r.complete();
    it = pending_reads_.erase(it);
  }
}

void Renderer::render(const gx::Frame& f, Image* capture_image) {
  if (!resize_efb(f.efb_width)) {
    std::fprintf(stderr, "[video] could not resize the EFB: %s\n", SDL_GetError());
    return;
  }
  SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
  if (!cmd) return;
  cmd_ = cmd;
  if (!upload(cmd, f)) std::fprintf(stderr, "[video] upload failed: %s\n", SDL_GetError());

  for (const gx::Command& c : f.commands) {
    switch (c.type) {
      case gx::Command::Type::Draw:
        begin_efb(cmd);
        draw(f, f.draws[c.index]);
        break;
      case gx::Command::Type::Clear:
        begin_efb(cmd);
        clear(f.clears[c.index]);
        break;
      case gx::Command::Type::Copy: copy(cmd, f.copies[c.index]); break;
      case gx::Command::Type::Readback: read_efb(cmd, f.readbacks[c.index]); break;
    }
  }
  end_pass();

  const auto xfb = f.xfb ? textures_.find(f.xfb) : textures_.end();
  if (xfb != textures_.end() && xfb->second.texture) {
    if (window_) present(cmd, &xfb->second, f.xfb_aspect);
    if (capture_image) capture(cmd, xfb->second);
    last_xfb_ = f.xfb;
    last_aspect_ = f.xfb_aspect;
  }

  // Submit, with a fence if downloads were recorded. Wait for it if the
  // guest thread is waiting for a readback, or for a screenshot.
  if (unfenced_reads_.empty() && !capture_.transfer) {
    SDL_SubmitGPUCommandBuffer(cmd);
  } else if (SDL_GPUFence* raw = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd)) {
    std::shared_ptr<SDL_GPUFence> fence(raw, [device = device_](SDL_GPUFence* p) { SDL_ReleaseGPUFence(device, p); });
    bool urgent = capture_.transfer != nullptr;
    for (PendingRead& r : unfenced_reads_) {
      urgent = urgent || r.request->urgent;
      r.fence = fence;
      pending_reads_.push_back(std::move(r));
    }
    unfenced_reads_.clear();
    if (urgent) {
      os::g_main_activity.set("waiting for the GPU (a readback)");
      SDL_WaitForGPUFences(device_, true, &raw, 1);
      os::g_main_activity.set("drawing a frame");
    }
    if (capture_image) finish_capture(capture_image);
  }
  poll_reads(false);

  for (u32 h : f.releases) {
    if (auto it = textures_.find(h); it != textures_.end()) {
      release_texture(it->second);
      textures_.erase(it);
    }
  }
  ++frames_rendered_;
}

SDL_GPUTexture* Renderer::pooled_texture(u32 width, u32 height, u32 levels) {
  // Released a few frames ago at least, so that the GPU is done with it.
  for (auto it = texture_pool_.begin(); it != texture_pool_.end(); ++it) {
    if (it->width == width && it->height == height && it->levels == levels && it->released + 4 <= frames_rendered_) {
      SDL_GPUTexture* t = it->texture;
      texture_pool_bytes_ -= it->bytes;
      texture_pool_.erase(it);
      return t;
    }
  }
  return nullptr;
}

void Renderer::release_texture(const Texture& t) {
  if (!t.texture) return;
  if (!t.uploaded) {
    SDL_ReleaseGPUTexture(device_, t.texture);
    return;
  }
  const u64 bytes = u64{t.width} * t.height * 4 * 4 / 3;  // with its mipmaps
  texture_pool_.push_back({t.texture, t.width, t.height, t.levels, frames_rendered_, bytes});
  texture_pool_bytes_ += bytes;
  constexpr u64 kPoolBytes = 256ull << 20;
  while (texture_pool_bytes_ > kPoolBytes && !texture_pool_.empty()) {
    SDL_ReleaseGPUTexture(device_, texture_pool_.front().texture);
    texture_pool_bytes_ -= texture_pool_.front().bytes;
    texture_pool_.pop_front();
  }
}

}  // namespace video
