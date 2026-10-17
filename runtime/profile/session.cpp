// SPDX-License-Identifier: GPL-3.0-or-later

#include "profile/session.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "os/thread_priority.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace profile {

namespace fs = std::filesystem;

namespace {

u64 steady_ns() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

u64 unix_ms() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::FILE* open_file(const fs::path& path, const char* mode) {
#ifdef _WIN32
  std::wstring wide_mode;
  for (const char* p = mode; *p; ++p) wide_mode += static_cast<wchar_t>(*p);
  return _wfopen(path.c_str(), wide_mode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

std::string read_text(const fs::path& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// The number after "key": in a JSON text (0 if absent).
u64 json_number(const std::string& text, const char* key) {
  const std::string quoted = std::string("\"") + key + "\":";
  const std::size_t at = text.find(quoted);
  return at == std::string::npos ? 0 : std::strtoull(text.c_str() + at + quoted.size(), nullptr, 10);
}

#ifdef _WIN32
int dup_fd(int fd) { return _dup(fd); }
int dup2_fd(int from, int to) { return _dup2(from, to); }
int close_fd(int fd) { return _close(fd); }
int read_fd(int fd, char* buf, unsigned n) { return _read(fd, buf, n); }
int write_fd(int fd, const char* buf, unsigned n) { return _write(fd, buf, n); }
bool make_pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT) == 0; }
void set_std_handles() {
  SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
  SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
}
#else
int dup_fd(int fd) { return dup(fd); }
int dup2_fd(int from, int to) { return dup2(from, to); }
int close_fd(int fd) { return close(fd); }
int read_fd(int fd, char* buf, unsigned n) { return static_cast<int>(read(fd, buf, n)); }
int write_fd(int fd, const char* buf, unsigned n) { return static_cast<int>(write(fd, buf, n)); }
bool make_pipe(int fds[2]) { return pipe(fds) == 0; }
void set_std_handles() {}
#endif

}  // namespace

Session::~Session() {
  if (open_) close({});
}

bool Session::open(const fs::path& dir, bool resume, const Json& header, const Json& run, std::string* error) {
  std::error_code ec;
  fs::create_directories(dir / "thumbs", ec);
  if (ec) {
    *error = "cannot create " + dir.string() + ": " + ec.message();
    return false;
  }
  dir_ = dir;
  const fs::path session_path = dir / "session.json";
  u64 started_ms = unix_ms();
  if (resume && fs::exists(session_path, ec)) {
    // Go on with the session's clock and the next run number.
    if (const u64 ms = json_number(read_text(session_path), "started_unix_ms")) started_ms = ms;
    run_ = static_cast<int>(json_number(read_text(dir / "run.json"), "run")) + 1;
  } else {
    Json j = header;
    j.add("started_unix_ms", started_ms);
    std::ofstream f(session_path, std::ios::binary | std::ios::trunc);
    f << j.str() << "\n";
    if (!f) {
      *error = "cannot write " + session_path.string();
      return false;
    }
  }
  {
    std::ofstream f(dir / "run.json", std::ios::binary | std::ios::trunc);
    f << Json().add("run", run_).str() << "\n";
  }
  origin_ns_ = steady_ns() - (unix_ms() - started_ms) * 1000000ull;

  frames_ = open_file(dir / "frames.bin", "ab");
  events_ = open_file(dir / "events.jsonl", "ab");
  system_ = open_file(dir / "system.jsonl", "ab");
  if (!frames_ || !events_ || !system_) {
    *error = "cannot write in " + dir.string();
    for (std::FILE* f : {frames_, events_, system_})
      if (f) std::fclose(f);
    frames_ = events_ = system_ = nullptr;
    return false;
  }
  // Large buffers: the writer thread flushes them every second.
  std::setvbuf(frames_, nullptr, _IOFBF, 1 << 18);
  std::setvbuf(events_, nullptr, _IOFBF, 1 << 16);
  open_ = true;
  stopping_ = false;
  writer_ = std::thread([this] { writer_loop(); });
  start_console_tee();
  event(resume ? "resume" : "start", run);
  return true;
}

void Session::close(const Json& summary) {
  if (!open_) return;
  event("end");
  {
    std::lock_guard lock(job_mutex_);
    stopping_ = true;
  }
  job_cv_.notify_all();
  if (writer_.joinable()) writer_.join();
  stop_console_tee();
  {
    std::lock_guard lock(file_mutex_);
    for (std::FILE* f : {frames_, events_, system_})
      if (f) std::fclose(f);
    frames_ = events_ = system_ = nullptr;
  }
  if (!summary.empty()) {
    std::ofstream f(dir_ / "summary.json", std::ios::binary | std::ios::trunc);
    f << summary.str() << "\n";
  }
  open_ = false;
}

u64 Session::now_us() const { return to_us(steady_ns()); }

u64 Session::to_us(u64 ns) const { return ns > origin_ns_ ? (ns - origin_ns_) / 1000 : 0; }

void Session::event(std::string_view type, const Json& fields) {
  if (!open_) return;
  char t[32];
  std::snprintf(t, sizeof(t), "%.3f", static_cast<double>(now_us()) * 1e-6);
  std::string line = "{\"t\":";
  line += t;
  line += ",\"f\":" + std::to_string(frame()) + ",\"e\":" + json_string(type);
  if (!fields.empty()) line += "," + fields.fields();
  line += "}\n";
  std::lock_guard lock(file_mutex_);
  if (events_) std::fwrite(line.data(), 1, line.size(), events_);
}

void Session::frame_record(const FrameRecord& r) {
  std::lock_guard lock(file_mutex_);
  if (!frames_) return;
  std::fwrite(&r.t_us, sizeof(r.t_us), 1, frames_);
  std::fwrite(&r.frame, sizeof(u32), kFrameFieldCount, frames_);
  frames_written_.fetch_add(1, std::memory_order_relaxed);
}

void Session::system_line(const Json& fields) {
  char t[32];
  std::snprintf(t, sizeof(t), "%.3f", static_cast<double>(now_us()) * 1e-6);
  std::string line = std::string("{\"t\":") + t + ",\"f\":" + std::to_string(frame());
  if (!fields.empty()) line += "," + fields.fields();
  line += "}\n";
  std::lock_guard lock(file_mutex_);
  if (system_) std::fwrite(line.data(), 1, line.size(), system_);
}

void Session::post(std::function<void()> job) {
  {
    std::lock_guard lock(job_mutex_);
    if (stopping_ || !open_) return;
    jobs_.push_back(std::move(job));
  }
  job_cv_.notify_one();
}

void Session::flush(bool wait) {
  std::unique_lock lock(file_mutex_, std::defer_lock);
  if (wait) lock.lock();
  else if (!lock.try_lock()) return;
  for (std::FILE* f : {frames_, events_, system_})
    if (f) std::fflush(f);
  if (console_log_) std::fflush(console_log_);
}

void Session::writer_loop() {
  os::set_thread_name("profile writer");
  auto next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::unique_lock lock(job_mutex_);
  for (;;) {
    job_cv_.wait_until(lock, next_flush, [this] { return stopping_ || !jobs_.empty(); });
    while (!jobs_.empty()) {
      auto job = std::move(jobs_.front());
      jobs_.pop_front();
      lock.unlock();
      job();
      lock.lock();
    }
    if (std::chrono::steady_clock::now() >= next_flush || stopping_) {
      lock.unlock();
      flush();
      lock.lock();
      next_flush = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    if (stopping_ && jobs_.empty()) return;
  }
}

void Session::start_console_tee() {
  console_log_ = open_file(dir_ / "console.log", "ab");
  if (!console_log_) return;
  std::fflush(stdout);
  std::fflush(stderr);
  for (int i = 0; i < 2; ++i) {
    Stream& s = streams_[i];
    int fds[2];
    if (!make_pipe(fds)) continue;
    s.fd = i + 1;
    s.pipe_read = fds[0];
    s.pipe_write = fds[1];
    s.saved = dup_fd(s.fd);
    dup2_fd(s.pipe_write, s.fd);
    s.thread = std::thread([this, &s] {
      os::set_thread_name("console log");
      char buf[4096];
      for (;;) {
        const int n = read_fd(s.pipe_read, buf, sizeof(buf));
        if (n <= 0) break;
        if (s.saved >= 0) write_fd(s.saved, buf, static_cast<unsigned>(n));
        std::lock_guard lock(file_mutex_);
        std::fwrite(buf, 1, static_cast<std::size_t>(n), console_log_);
      }
    });
  }
  set_std_handles();
}

void Session::stop_console_tee() {
  if (!console_log_) return;
  std::fflush(stdout);
  std::fflush(stderr);
  // Put the streams back; with each pipe's last writer closed, its thread
  // reads what is left and stops.
  for (Stream& s : streams_) {
    if (s.fd < 0) continue;
    if (s.saved >= 0) dup2_fd(s.saved, s.fd);
    close_fd(s.pipe_write);
  }
  set_std_handles();
  for (Stream& s : streams_) {
    if (s.fd < 0) continue;
    if (s.thread.joinable()) s.thread.join();
    close_fd(s.pipe_read);
    if (s.saved >= 0) close_fd(s.saved);
    s = Stream{};
  }
  std::lock_guard lock(file_mutex_);
  std::fclose(console_log_);
  console_log_ = nullptr;
}

}  // namespace profile
