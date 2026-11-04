// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#include "app/launcher.h"
#include "app/paths.h"
#include "camera/jpeg.h"
#include "gx/describe.h"
#include "hw/vi.h"
#include "machine.h"
#include "video/renderer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace app {

namespace fs = std::filesystem;
using namespace gekko;
using profile::Json;
using profile::JsonArray;

namespace {

u32 us(u64 ns) { return static_cast<u32>(std::min<u64>(ns / 1000, 0xFFFFFFFFu)); }
u32 kb(u64 bytes) { return static_cast<u32>(std::min<u64>(bytes / 1024, 0xFFFFFFFFu)); }
u32 clamp_count(u32 v, u32 bits) { return std::min(v, (1u << bits) - 1); }

std::string read_text(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// The source tree's commit, if OpenGP runs from one ("" if not).
std::string git_commit() {
  fs::path git = home_dir() / ".git";
  std::error_code ec;
  if (fs::is_regular_file(git, ec)) {  // a worktree: "gitdir: <path>"
    const std::string text = trim(read_text(git));
    if (text.rfind("gitdir: ", 0) != 0) return {};
    git = fs::path(text.substr(8));
  }
  fs::path common = git;
  if (const std::string c = trim(read_text(git / "commondir")); !c.empty()) common = fs::path(c).is_absolute() ? fs::path(c) : git / c;
  const std::string head = trim(read_text(git / "HEAD"));
  if (head.rfind("ref: ", 0) != 0) return head.substr(0, 12);
  const std::string ref = head.substr(5);
  for (const fs::path& dir : {git, common})
    if (std::string hash = trim(read_text(dir / ref)); !hash.empty()) return hash.substr(0, 12) + " (" + ref.substr(ref.rfind('/') + 1) + ")";
  std::istringstream packed(read_text(common / "packed-refs"));
  for (std::string line; std::getline(packed, line);)
    if (line.size() > 41 && trim(line.substr(41)) == ref) return line.substr(0, 12) + " (" + ref.substr(ref.rfind('/') + 1) + ")";
  return {};
}

Json system_info() {
  Json j;
  j.add("cores", static_cast<int>(std::thread::hardware_concurrency()));
#ifdef _WIN32
  using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
  const auto get_version = reinterpret_cast<GetVersion>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW v{};
  v.dwOSVersionInfoSize = sizeof(v);
  if (get_version && get_version(&v) == 0)
    j.add("os", "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
                    std::to_string(v.dwBuildNumber));
  char cpu[256] = {};
  DWORD size = sizeof(cpu);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                   RRF_RT_REG_SZ, nullptr, cpu, &size) == ERROR_SUCCESS)
    j.add("cpu", trim(cpu));
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) j.add("ram_mb", static_cast<u64>(memory.ullTotalPhys >> 20));
#else
  utsname u{};
  if (uname(&u) == 0) j.add("os", std::string(u.sysname) + " " + u.release + " " + u.machine);
#ifdef __APPLE__
  char cpu[256] = {};
  std::size_t size = sizeof(cpu);
  if (sysctlbyname("machdep.cpu.brand_string", cpu, &size, nullptr, 0) == 0) j.add("cpu", std::string(cpu));
  u64 ram = 0;
  size = sizeof(ram);
  if (sysctlbyname("hw.memsize", &ram, &size, nullptr, 0) == 0) j.add("ram_mb", ram >> 20);
#else
  std::istringstream info(read_text("/proc/cpuinfo"));
  for (std::string line; std::getline(info, line);) {
    if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
      j.add("cpu", trim(line.substr(line.find(':') + 2)));
      break;
    }
  }
  j.add("ram_mb", static_cast<u64>(sysconf(_SC_PHYS_PAGES)) * static_cast<u64>(sysconf(_SC_PAGESIZE)) >> 20);
#endif
#endif
#ifdef NDEBUG
  j.add("build", "release");
#else
  j.add("build", "debug");
#endif
#ifdef __clang__
  j.add("compiler", "clang " __clang_version__);
#elif defined(__GNUC__)
  j.add("compiler", "gcc " __VERSION__);
#elif defined(_MSC_VER)
  j.add("compiler", "msvc " + std::to_string(_MSC_VER));
#endif
  return j;
}

Json settings_json(const Settings& s) {
  return Json()
      .add("fullscreen", s.fullscreen)
      .add("window", std::to_string(s.window_width) + "x" + std::to_string(s.window_height))
      .add("aspect", aspect_name(s.aspect))
      .add("internal_resolution", s.internal_resolution)
      .add("vsync", s.vsync)
      .add("even_pacing", s.even_pacing)
      .add("volume", s.volume)
      .add("free_play", s.free_play)
      .add("pause_in_menu", s.pause_in_menu)
      .add("pause_in_background", s.pause_in_background)
      .add("card", s.card)
      .add("camera", camera_name(s.camera))
      .add("rich_presence", s.rich_presence)
      .add("cabinet", s.cabinet)
      .add("lan", s.lan);
}

const char* risk_name(gx::Risk::Kind kind) {
  using K = gx::Risk::Kind;
  switch (kind) {
    case K::TextureOutOfRange: return "texture_out_of_range";
    case K::TextureFormat: return "texture_format";
    case K::TmemRgba8: return "tmem_rgba8";
    case K::TmemOutOfRange: return "tmem_out_of_range";
    case K::CopySize: return "efb_copy_size";
    case K::CopyFormat: return "efb_copy_format";
    case K::EfbPeekEmpty: return "efb_peek_empty";
    case K::EfbWrite: return "efb_write";
    case K::ZFreeze: return "zfreeze";
    case K::DisplayListRange: return "display_list_out_of_range";
  }
  return "?";
}

const char* risk_text(gx::Risk::Kind kind) {
  using K = gx::Risk::Kind;
  switch (kind) {
    case K::TextureOutOfRange: return "a texture outside guest memory (drawn white)";
    case K::TextureFormat: return "an invalid texture format (drawn white)";
    case K::TmemRgba8: return "a preloaded RGBA8 texture (not supported; drawn white)";
    case K::TmemOutOfRange: return "a preloaded texture past the end of TMEM (drawn white)";
    case K::CopySize: return "a texture where an EFB copy of another size went (reads memory the copy never wrote)";
    case K::CopyFormat: return "a texture read from an EFB copy in another format";
    case K::EfbPeekEmpty: return "a CPU read of the EFB before any readback was ready (reads 0)";
    case K::EfbWrite: return "a CPU write to the EFB (not supported)";
    case K::ZFreeze: return "z-freeze (not supported)";
    case K::DisplayListRange: return "a display list outside guest memory";
  }
  return "?";
}

const char* scene_kind(GameStatus::Scene scene) {
  switch (scene) {
    case GameStatus::Scene::Starting: return "starting";
    case GameStatus::Scene::Attract: return "attract";
    case GameStatus::Scene::Menus: return "menus";
    case GameStatus::Scene::Racing: return "racing";
    case GameStatus::Scene::Finished: return "finished";
    case GameStatus::Scene::TestMenu: return "test menu";
  }
  return "?";
}

// p-th percentile (0-1) of `v` (sorted in place), in milliseconds.
double percentile_ms(std::vector<u32>& v, double p) {
  if (v.empty()) return 0;
  const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
  return v[i] / 1000.0;
}

Json times_json(std::vector<u32> v) {
  if (v.empty()) return Json();
  double sum = 0;
  for (u32 x : v) sum += x;
  Json j;
  j.add("mean", sum / static_cast<double>(v.size()) / 1000.0)
      .add("p50", percentile_ms(v, 0.5))
      .add("p90", percentile_ms(v, 0.9))
      .add("p99", percentile_ms(v, 0.99))
      .add("p99_9", percentile_ms(v, 0.999))
      .add("max", *std::max_element(v.begin(), v.end()) / 1000.0);
  return j;
}

std::string timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
  return buf;
}

}  // namespace

Profiler::Profiler(opengp::Machine& machine, const Game& game, video::Renderer& renderer, Options options)
    : machine_(machine), game_(game), renderer_(renderer), options_(options) {}

Profiler::~Profiler() { stop("exit"); }

bool Profiler::start(fs::path dir, const fs::path& profiles_dir, bool resume, std::string* error) {
  std::lock_guard lock(mutex_);
  if (session_.is_open()) return true;
  if (dir.empty()) dir = profiles_dir / timestamp();

  JsonArray fields;
#define OPENGP_FRAME_FIELD(name, doc) fields.add(Json().add("name", #name).add("doc", doc));
  OPENGP_FRAME_FIELDS(OPENGP_FRAME_FIELD)
#undef OPENGP_FRAME_FIELD
  Json header;
  header.add("format", 1)
      .add("opengp", OPENGP_VERSION)
      .add("commit", git_commit())
      .add("game", Json().add("id", game_.id).add("title", game_.title))
      .add("system", system_info())
      .add("frame_record", Json()
                               .add("struct", "<Q" + std::to_string(profile::kFrameFieldCount) + "I")
                               .add("size", 8 + 4 * profile::kFrameFieldCount)
                               .add("time", "t_us: u64, microseconds since the session started, when presented")
                               .raw("fields", fields.str()));
  Json run;
  run.add("frame_offset", machine_.frame_offset())
      .add("gpu", Json()
                      .add("api", renderer_.driver())
                      .add("name", renderer_.device_name())
                      .add("driver", renderer_.driver_version()))
      .add("sampling_hz", options_.rate_hz)
      .add("gpu_sync", std::getenv("OPENGP_PROFILE_GPU_SYNC") != nullptr);
  if (describe_) {
    const Json host = describe_();
    if (!host.empty()) {
      header.raw("host", host.str());
      run.raw("host", host.str());
    }
  }
  if (!session_.open(dir, resume, header, run, error)) return false;
  monitor_.start(session_, {options_.rate_hz});
  frames_ = hitches_ = marks_ = drops_ = pipelines_ = pipeline_binds_ = max_texture_kb_ = 0;
  auto_captures_ = 0;
  last_auto_capture_us_ = 0;
  capture_request_ = false;
  guest_times_.clear();
  busy_times_.clear();
  present_times_.clear();
  hitch_causes_.clear();
  scenes_.clear();
  risks_.clear();
  recent_.clear();
  recent_next_ = 0;
  have_status_ = have_settings_ = false;
  last_flags_ = 0;
  last_present_ns_ = last_retrace_ = 0;
  have_retrace_ = false;
  last_underruns_ = 0;
  next_thumbnail_ = 0;
  thumbnail_now_ = true;
  live_ = {};
  live_graph_.clear();
  live_hitches_ = 0;
  live_present_avg_us_ = 0;
  last_live_present_ns_ = last_live_retrace_ = last_live_idle_ns_ = 0;
  have_live_retrace_ = false;
  renderer_.take_stats();
  renderer_.take_thumbnails();
  std::fprintf(stderr, "[profile] recording to %s\n", dir.string().c_str());
  return true;
}

void Profiler::stop(const char* why) {
  std::lock_guard lock(mutex_);
  if (!session_.is_open()) return;
  write_thumbnails();
  monitor_.stop();
  const Json s = summary(why);
  const u64 frames = frames_;
  std::fprintf(stderr, "[profile] stopped (%s): %llu frames, %llu hitches in %s\n", why,
               static_cast<unsigned long long>(frames), static_cast<unsigned long long>(hitches_),
               session_.dir().string().c_str());
  session_.event("stop", Json().add("why", why));
  session_.close(s);
}

void Profiler::mark(const std::string& note) {
  std::lock_guard lock(mutex_);
  if (!session_.is_open()) return;
  ++marks_;
  mark_pending_ = true;
  thumbnail_now_ = true;
  session_.event("mark", Json().add("n", marks_).add("note", note));
}

void Profiler::note(std::string_view type, const Json& fields) {
  std::lock_guard lock(mutex_);
  session_.event(type, fields);
}

void Profiler::write_thumbnails() {
  for (video::Thumbnail& t : renderer_.take_thumbnails()) {
    if (!session_.is_open()) continue;
    const u64 number = machine_.frame_offset() + t.frame;
    const fs::path path = session_.dir() / "thumbs" / ("f" + std::to_string(number) + ".jpg");
    session_.post([path, image = std::move(t.image)] {
      std::vector<u8> rgb(std::size_t{image.width} * image.height * 3);
      for (std::size_t i = 0, n = std::size_t{image.width} * image.height; i < n; ++i) {
        rgb[3 * i] = image.rgba[4 * i];
        rgb[3 * i + 1] = image.rgba[4 * i + 1];
        rgb[3 * i + 2] = image.rgba[4 * i + 2];
      }
      const std::vector<u8> jpeg =
          camera::encode_jpeg(rgb.data(), static_cast<int>(image.width), static_cast<int>(image.height), 80);
      std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    });
  }
}

void Profiler::on_frame(const gx::Frame& frame, const FrameInfo& info) {
  std::lock_guard lock(mutex_);
  const video::RenderStats rs = renderer_.take_stats();
  const hw::ViDevice* vi = machine_.hardware().device<hw::ViDevice>();
  const u64 retrace = vi ? vi->retrace_count() : 0;
  update_live(frame, info, rs, retrace);
  if (!session_.is_open()) return;
  write_thumbnails();
  session_.set_frame(info.number);
  const gx::FrameStats& g = frame.stats;
  profile::FrameRecord r;
  r.t_us = session_.now_us();
  r.frame = static_cast<u32>(info.number);
  r.retrace = static_cast<u32>(retrace);

  // The game's state.
  GameStatus status;
  if (game_.status) game_.status(machine_.base(), &status);
  r.scene = status.scene_id;
  r.race = static_cast<u32>(status.position & 0xFF) | (static_cast<u32>(status.lap & 0xFF) << 8) |
           (static_cast<u32>(status.laps & 0xFF) << 16) | (u32{status.started} << 24) | (u32{status.finished} << 25);
  scene_events(status);
  settings_events(info);
  if (frame.efb_width != last_efb_width_ || info.scale != last_scale_) {
    session_.event("efb", Json().add("width", frame.efb_width).add("scale", info.scale));
    last_efb_width_ = frame.efb_width;
    last_scale_ = info.scale;
  }

  // The game's thread.
  const u64 idle_ns = machine_.threads().idle_ns();
  const u64 exceptions = machine_.exceptions_delivered();
  r.guest_us = us(g.interval_ns);
  r.idle_us = frames_ ? us(idle_ns - last_idle_ns_) : 0;
  r.wait_us = us(g.wait_ns);
  r.readback_wait_us = us(g.readback_ns);
  r.guest_busy_us = r.guest_us > r.idle_us + r.wait_us + r.readback_wait_us
                        ? r.guest_us - r.idle_us - r.wait_us - r.readback_wait_us
                        : 0;
  r.gx_us = us(g.draw_ns);
  r.gx_state_us = us(g.state_ns);
  r.gx_texture_us = us(g.texture_ns);
  r.gx_decode_us = us(g.decode_ns);
  r.gx_transform_us = us(g.transform_ns);
  r.calls = g.calls;
  r.culled = g.culled;
  r.draws = static_cast<u32>(frame.draws.size());
  r.vertices = g.vertices;
  r.triangles = g.triangles;
  r.states = static_cast<u32>(frame.states.size());
  r.binds = g.binds;
  r.decodes = g.decodes;
  r.decoded_kb = kb(g.decoded_bytes);
  r.hashed_kb = kb(g.hashed_bytes);
  r.copy_binds = g.copy_binds;
  r.copies = g.copies;
  r.clears = g.clears;
  r.partials = g.partials;
  r.readbacks = g.readbacks;
  r.cached_textures = g.cached_textures;
  r.depth_equal = g.depth_equal;
  r.widescreen = clamp_count(g.widened_2d, 10) | (clamp_count(g.extended_edges, 10) << 10) |
                 (clamp_count(g.widened_overlays, 10) << 20);
  r.risks = g.risks;
  r.exceptions = frames_ ? static_cast<u32>(exceptions - last_exceptions_) : 0;
  last_idle_ns_ = idle_ns;
  last_exceptions_ = exceptions;

  // The renderer and the main thread.
  r.render_us = us(rs.render_ns);
  r.upload_us = us(rs.upload_ns);
  r.upload_kb = kb(rs.upload_bytes);
  r.acquire_us = us(rs.acquire_ns);
  r.submit_us = us(rs.submit_ns);
  r.fence_us = us(rs.fence_ns);
  r.gpu_us = us(rs.gpu_ns);
  r.pipeline_us = us(rs.pipeline_ns);
  r.pipelines = rs.pipelines;
  r.textures = rs.textures;
  r.texture_kb = kb(rs.texture_bytes);
  r.gpu_draws = rs.draws;
  r.passes = rs.passes;
  r.pipeline_binds = rs.pipeline_binds;
  r.live_textures = renderer_.live_textures();
  r.live_texture_kb = kb(renderer_.live_texture_bytes());
  r.live_pipelines = renderer_.live_pipelines();
  pipeline_binds_ += rs.pipeline_binds;
  max_texture_kb_ = std::max<u64>(max_texture_kb_, r.live_texture_kb);
  r.queue_us = us(info.queue_ns);
  r.queue_depth = info.queue_depth;
  r.pace_us = us(info.pace_ns);
  const u64 now_ns = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  r.latency_us = g.finished_ns ? us(now_ns - g.finished_ns) : 0;
  r.present_us = last_present_ns_ ? us(now_ns - last_present_ns_) : 0;
  last_present_ns_ = now_ns;
  r.audio_queue_ms10 = static_cast<u32>(std::max(0.0, info.audio_queue_ms * 10.0));
  r.audio_underruns = frames_ ? static_cast<u32>(info.audio_underruns - last_underruns_) : 0;
  last_underruns_ = info.audio_underruns;

  r.flags = (info.paused ? profile::kPaused : 0) | (info.menu_open ? profile::kMenuOpen : 0) |
            (info.background ? profile::kBackground : 0) | (info.linked ? profile::kLinked : 0) |
            (info.settings && info.settings->vsync ? profile::kVsync : 0) |
            (info.settings && info.settings->fullscreen ? profile::kFullscreen : 0) |
            (info.settings && info.settings->even_pacing ? profile::kEvenPacing : 0);
  const u32 changed = r.flags ^ last_flags_;
  auto edge = [&](u32 flag, const char* on, const char* off) {
    if (frames_ && (changed & flag)) session_.event((r.flags & flag) ? on : off);
  };
  edge(profile::kPaused, "paused", "unpaused");
  edge(profile::kMenuOpen, "menu_opened", "menu_closed");
  edge(profile::kBackground, "background", "foreground");
  edge(profile::kLinked, "linked", "unlinked");
  last_flags_ = r.flags;
  // Retraces that went by without a frame: a pause (or the menu) makes many,
  // and they are not the game's; the first frame after one is not late.
  if (have_retrace_ && !info.paused && !info.menu_open && retrace > last_retrace_) {
    r.drops = static_cast<u32>(std::min<u64>(retrace - last_retrace_ - 1, 0xFFFFFFFFu));
    drops_ += r.drops;
  }
  last_retrace_ = retrace;
  have_retrace_ = true;
  if (mark_pending_) r.flags |= profile::kMark, mark_pending_ = false;
  if (frame.capture) r.flags |= profile::kCapture;

  // Likely glitches: an event the first time each one is seen, counts in
  // the summary.
  for (const gx::Risk& risk : frame.risks) {
    RiskStats& stats = risks_[{static_cast<int>(risk.kind), risk.address, risk.a, risk.b}];
    if (stats.count++ == 0) {
      stats.first_frame = info.number;
      stats.first_t = static_cast<double>(r.t_us) * 1e-6;
      char address[16];
      std::snprintf(address, sizeof(address), "%08X", risk.address);
      session_.event("risk", Json()
                                 .add("kind", risk_name(risk.kind))
                                 .add("what", risk_text(risk.kind))
                                 .add("address", address)
                                 .add("a", risk.a)
                                 .add("b", risk.b)
                                 .add("scene", status.scene_id)
                                 .add("course", status.course));
      thumbnail_now_ = true;
      maybe_auto_capture();
    }
  }
  if (g.risks > frame.risks.size()) {
    // More than the frame kept: counted, not described.
    risks_[{-1, 0, 0, 0}].count += g.risks - frame.risks.size();
  }

  // Shader compiles (each pipeline is made once, on first use).
  if (!rs.new_pipelines.empty()) {
    pipelines_ += rs.new_pipelines.size();
    JsonArray list;
    for (const u32 key : rs.new_pipelines) {
      char hex[16];
      std::snprintf(hex, sizeof(hex), "%06X", key);
      list.add(Json().add("key", hex).add("state", gx::describe_pipeline(key)));
    }
    session_.event("pipelines", Json()
                                    .add("count", rs.new_pipelines.size())
                                    .add("ms", static_cast<double>(rs.pipeline_ns) * 1e-6)
                                    .add("scene", status.scene_id)
                                    .raw("created", list.str()));
  }
  if (rs.failures) session_.event("gpu_failures", Json().add("count", rs.failures));

  // Hitches: a frame much longer than the frames before it.
  const bool steady = !info.paused && !info.menu_open && frames_since_pause_ > 30;
  frames_since_pause_ = (info.paused || info.menu_open) ? 0 : frames_since_pause_ + 1;
  ++frames_since_scene_;
  u32 expected = 16683;  // 59.94 frames a second
  if (recent_.size() >= 30) {
    std::vector<u32> sorted = recent_;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    expected = sorted[sorted.size() / 2];
  }
  const u32 threshold = std::max(expected + expected / 2, expected + 8000);
  if (frames_ && steady && (r.guest_us > threshold || r.present_us > threshold)) {
    r.flags |= profile::kHitch;
    hitch(frame, r, rs, expected);
    maybe_auto_capture();
  }
  if (steady && r.guest_us) {
    if (recent_.size() < 120) recent_.push_back(r.guest_us);
    else recent_[recent_next_++ % recent_.size()] = r.guest_us;
  }

  // Thumbnails: now and then, and of anything worth seeing.
  if ((options_.thumbnail_every > 0 && info.number >= next_thumbnail_) || thumbnail_now_) {
    renderer_.request_thumbnail(options_.thumbnail_width);
    r.flags |= profile::kThumbnail;
    thumbnail_now_ = false;
    next_thumbnail_ = info.number + static_cast<u64>(std::max(options_.thumbnail_every, 1));
    if (options_.thumbnail_every <= 0) next_thumbnail_ = ~0ull;
  }

  session_.frame_record(r);
  ++frames_;
  if (!info.paused) {
    guest_times_.push_back(r.guest_us);
    busy_times_.push_back(r.guest_busy_us);
    present_times_.push_back(r.present_us);
    SceneStats& scene = scenes_[status.scene_id];
    scene.kind = scene_kind(status.scene);
    ++scene.frames;
    scene.guest_us += r.guest_us;
    scene.times.push_back(r.guest_us);
    if (r.flags & profile::kHitch) ++scene.hitches;
  }
}

// The numbers the overlay shows, kept even when not recording: a player can
// watch the game's frame times and the computer's load before deciding to
// record, and see the profile's size while it records.
void Profiler::update_live(const gx::Frame& frame, const FrameInfo& info, const video::RenderStats& rs, u64 retrace) {
  LiveStats& l = live_;
  l.recording = session_.is_open();
  l.auto_capture = auto_capture_;
  // The frame being handled is counted next: show it now.
  l.frames = l.recording ? frames_ + 1 : 0;
  l.marks = l.recording ? marks_ : 0;
  if (l.recording) l.seconds = static_cast<double>(session_.now_us()) * 1e-6;

  const gx::FrameStats& g = frame.stats;
  const u64 guest_us = us(g.interval_ns);
  const u64 idle_total = machine_.threads().idle_ns();
  const u64 idle_us = last_live_idle_ns_ ? us(idle_total - last_live_idle_ns_) : 0;
  last_live_idle_ns_ = idle_total;
  const u64 wait_us = us(g.wait_ns), readback_us = us(g.readback_ns);
  const u64 busy_us = guest_us > idle_us + wait_us + readback_us ? guest_us - idle_us - wait_us - readback_us : 0;
  const auto now_ns = static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  const u64 present_us = last_live_present_ns_ ? us(now_ns - last_live_present_ns_) : 0;
  last_live_present_ns_ = now_ns;

  l.guest_ms = static_cast<float>(guest_us) / 1000.0f;
  l.busy_ms = static_cast<float>(busy_us) / 1000.0f;
  l.present_ms = static_cast<float>(present_us) / 1000.0f;
  l.render_ms = static_cast<float>(us(rs.render_ns)) / 1000.0f;
  l.audio_ms = static_cast<float>(info.audio_queue_ms);
  l.audio_underruns = info.audio_underruns;
  l.textures = renderer_.live_textures();
  l.pipelines = renderer_.live_pipelines();
  l.texture_mb = static_cast<double>(renderer_.live_texture_bytes()) / (1024.0 * 1024.0);

  // Hitches and drops, as for a recording, but only while playing: a pause
  // or the menu keeps retraces coming without new frames.
  const bool steady = !info.paused && !info.menu_open;
  float expected = 16.683f;
  if (live_graph_.size() >= 30) {
    std::vector<float> sorted = live_graph_;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    expected = sorted[sorted.size() / 2];
  }
  const float slow = std::max(expected * 1.5f, expected + 8.0f);
  if (steady && (l.guest_ms > slow || l.present_ms > slow)) ++live_hitches_;
  l.hitches = l.recording ? hitches_ : live_hitches_;
  if (steady) {
    if (have_live_retrace_ && retrace > last_live_retrace_)
      l.drops += std::min<u64>(retrace - last_live_retrace_ - 1, 0xFFFFFFFFu);
    live_graph_.push_back(l.guest_ms);
    if (live_graph_.size() > 120) live_graph_.erase(live_graph_.begin());
    if (present_us) live_present_avg_us_ = live_present_avg_us_ > 0 ? live_present_avg_us_ * 0.9 + present_us * 0.1 : present_us;
  }
  last_live_retrace_ = retrace;
  have_live_retrace_ = true;
  if (live_graph_.empty()) {
    l.p99_ms = 0;
  } else {
    std::vector<float> sorted = live_graph_;
    const std::size_t i = static_cast<std::size_t>(0.99f * static_cast<float>(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(i), sorted.end());
    l.p99_ms = sorted[i];
  }
  l.fps = live_present_avg_us_ > 0 ? static_cast<float>(1e6 / live_present_avg_us_) : 0;
  l.system_cpu = monitor_.system_cpu() >= 0 ? static_cast<float>(monitor_.system_cpu() * 100.0) : -1.0f;
  if (have_status_) {
    l.scene = last_status_.activity.empty() ? scene_kind(last_status_.scene) : last_status_.activity;
    if (!last_status_.mode.empty()) l.scene += " · " + last_status_.mode;
    l.course = last_status_.course;
  }
  l.graph = live_graph_;
}

void Profiler::maybe_auto_capture() {
  if (!auto_capture_ || !session_.is_open() || capture_request_) return;
  const u64 now = session_.now_us();
  if (last_auto_capture_us_ && now - last_auto_capture_us_ < 5'000'000) return;
  last_auto_capture_us_ = now;
  capture_request_ = true;
  ++auto_captures_;
}

Profiler::LiveStats Profiler::live() const {
  std::lock_guard lock(mutex_);
  return live_;
}

void Profiler::set_auto_capture(bool on) {
  std::lock_guard lock(mutex_);
  auto_capture_ = on;
  live_.auto_capture = on;
}

bool Profiler::auto_capture() const {
  std::lock_guard lock(mutex_);
  return auto_capture_;
}

bool Profiler::take_capture_request() {
  std::lock_guard lock(mutex_);
  const bool requested = capture_request_;
  capture_request_ = false;
  return requested;
}

void Profiler::scene_events(const GameStatus& s) {
  const GameStatus& last = last_status_;
  if (!have_status_ || s.scene_id != last.scene_id || s.activity != last.activity) {
    Json j;
    j.add("scene", s.scene_id).add("kind", scene_kind(s.scene));
    if (!s.activity.empty()) j.add("activity", s.activity);
    if (!s.mode.empty()) j.add("mode", s.mode);
    if (!s.engine.empty()) j.add("engine", s.engine);
    if (!s.character.empty()) j.add("character", s.character);
    if (!s.cup.empty()) j.add("cup", s.cup);
    if (!s.course.empty()) j.add("course", s.course);
    if (s.round) j.add("round", s.round);
    session_.event("scene", j);
    frames_since_scene_ = 0;
    thumbnail_now_ = true;
  }
  if (have_status_ && s.started != last.started && s.started)
    session_.event("race_start", Json().add("course", s.course).add("character", s.character).add("mode", s.mode));
  if (have_status_ && s.lap != last.lap && s.lap > 0 && s.started)
    session_.event("lap", Json().add("lap", s.lap).add("laps", s.laps).add("position", s.position));
  if (have_status_ && s.finished != last.finished && s.finished)
    session_.event("race_end", Json().add("position", s.position).add("course", s.course));
  last_status_ = s;
  have_status_ = true;
}

void Profiler::settings_events(const FrameInfo& info) {
  if (!info.settings) return;
  if (have_settings_ && *info.settings == last_settings_) return;
  session_.event("settings", settings_json(*info.settings));
  last_settings_ = *info.settings;
  have_settings_ = true;
}

void Profiler::hitch(const gx::Frame& frame, const profile::FrameRecord& r, const video::RenderStats& rs,
                     u32 expected_us) {
  ++hitches_;
  const gx::FrameStats& g = frame.stats;
  // Where the extra time went: the largest part of the frame's time.
  const u64 busy = r.guest_us > r.idle_us + r.wait_us + r.readback_wait_us
                       ? r.guest_us - r.idle_us - r.wait_us - r.readback_wait_us
                       : 0;
  const u64 render_other = r.render_us > r.acquire_us + r.pipeline_us + r.upload_us
                               ? r.render_us - r.acquire_us - r.pipeline_us - r.upload_us
                               : 0;
  const std::pair<const char*, u64> parts[] = {
      {"game code", busy > r.gx_us ? busy - r.gx_us : 0},
      {"texture decoding", r.gx_decode_us},
      {"GX front end", r.gx_us > r.gx_decode_us ? r.gx_us - r.gx_decode_us : 0},
      {"waiting for the renderer", r.wait_us},
      {"EFB readbacks", r.readback_wait_us},
      {"shader compiles", r.pipeline_us},
      {"waiting for the display or GPU", r.acquire_us},
      {"uploads to the GPU", r.upload_us},
      {"renderer", render_other},
  };
  const auto* top = std::max_element(std::begin(parts), std::end(parts),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
  const u32 longest = std::max(r.guest_us, r.present_us);
  const u64 excess = longest > expected_us ? longest - expected_us : 0;
  std::string cause = top->first;
  if (top->second * 2 < excess) cause = "unexplained (the OS? another program?)";
  ++hitch_causes_[cause];

  Json breakdown;
  for (const auto& [name, value] : parts)
    if (value >= 500) breakdown.add(name, static_cast<double>(value) / 1000.0);
  if (r.idle_us) breakdown.add("game idle", r.idle_us / 1000.0);
  Json j;
  j.add("guest_ms", r.guest_us / 1000.0)
      .add("present_ms", r.present_us / 1000.0)
      .add("expected_ms", expected_us / 1000.0)
      .add("cause", cause)
      .add("ms", breakdown)
      .add("scene", r.scene)
      .add("after_scene_change", frames_since_scene_ < 10)
      .add("decodes", g.decodes)
      .add("pipelines", rs.pipelines);
  if (const double system = monitor_.system_cpu(); system >= 0) j.add("system_cpu", system * 100.0);
  // The game's busiest functions during the frame (from the stack samples).
  const u64 end = session_.to_us(g.finished_ns), begin = end > r.guest_us ? end - r.guest_us : 0;
  auto list = [](const std::vector<profile::Monitor::Hot>& hot) {
    JsonArray a;
    for (const auto& h : hot) a.raw("[" + profile::json_string(h.name) + "," + std::to_string(static_cast<int>(h.share * 100 + 0.5)) + "]");
    return a.str();
  };
  if (const auto self = monitor_.hottest(begin, end, 6, false); !self.empty()) {
    j.raw("self", list(self));
    j.raw("inclusive", list(monitor_.hottest(begin, end, 8, true)));
  }
  session_.event("hitch", j);
  thumbnail_now_ = true;
}

Json Profiler::summary(const char* why) const {
  Json j;
  j.add("why", why).add("run", session_.run()).add("frames", frames_);
  j.add("seconds", static_cast<double>(session_.now_us()) * 1e-6);
  j.add("guest_ms", times_json(guest_times_)).add("present_ms", times_json(present_times_));
  j.add("guest_busy_ms", times_json(busy_times_));
  j.add("hitches", hitches_).add("drops", drops_);
  j.add("pipeline_binds", pipeline_binds_).add("auto_captures", auto_captures_);
  Json gpu;
  gpu.add("textures", live_.textures)
      .add("texture_mb", live_.texture_mb)
      .add("max_texture_mb", static_cast<double>(max_texture_kb_) / 1024.0)
      .add("pipelines", live_.pipelines);
  j.add("gpu_now", gpu);
  Json causes;
  for (const auto& [cause, n] : hitch_causes_) causes.add(cause, n);
  j.add("hitch_causes", causes);
  JsonArray scenes;
  for (const auto& [id, s] : scenes_) {
    std::vector<u32> times = s.times;
    scenes.add(Json()
                   .add("scene", id)
                   .add("kind", s.kind)
                   .add("frames", s.frames)
                   .add("mean_ms", s.frames ? static_cast<double>(s.guest_us) / static_cast<double>(s.frames) / 1000.0 : 0.0)
                   .add("p99_ms", percentile_ms(times, 0.99))
                   .add("hitches", s.hitches));
  }
  j.raw("scenes", scenes.str());
  JsonArray risks;
  for (const auto& [key, s] : risks_) {
    const auto& [kind, address, a, b] = key;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%08X", address);
    risks.add(Json()
                  .add("kind", kind < 0 ? "more (not described)" : risk_name(static_cast<gx::Risk::Kind>(kind)))
                  .add("address", hex)
                  .add("a", a)
                  .add("b", b)
                  .add("count", s.count)
                  .add("first_frame", s.first_frame));
  }
  j.raw("risks", risks.str());
  j.add("pipelines_created", pipelines_).add("marks", marks_);
  return j;
}

}  // namespace app
