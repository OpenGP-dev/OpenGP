// SPDX-License-Identifier: GPL-3.0-or-later
//
// A profile: everything recorded about one play session, in a folder
// (user/profiles/<date>_<time>/ by default), made to be read by tools
// (tools/profiles.py) rather than by hand:
//
//   session.json   what was played, on what, with which settings; the record layouts
//   frames.bin     a FrameRecord per displayed frame (profile/record.h)
//   events.jsonl   one JSON object a line, in time order: scenes, hitches, likely
//                  glitches, shader compiles, marks, settings changes... ("e" says which)
//   system.jsonl   once a second: CPU use by thread, the whole computer's, memory
//   console.log    everything printed to the console
//   cpu/run<N>/    call stack samples (see profile/monitor.h)
//   thumbs/        small pictures of the screen, f<frame>.jpg
//   captures/      frame captures (F9)
//   summary.json   written when the session ends
//
// Times in events are seconds since the session started ("t"), and "f" is the
// latest displayed frame. A game that restarts itself (its test menu) goes on
// recording into the same profile (resume).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "gekko/types.h"
#include "profile/json.h"
#include "profile/record.h"

namespace profile {

using namespace gekko;

class Session {
 public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Creates the profile in `dir`, or opens it to go on recording (`resume`).
  // `header` holds session.json's fields (a new profile only); `run` describes
  // this process (the "run" event).
  bool open(const std::filesystem::path& dir, bool resume, const Json& header, const Json& run,
            std::string* error);
  // Ends the session: finishes the queued work, writes summary.json (if
  // `summary` is not empty), closes the files.
  void close(const Json& summary);
  bool is_open() const { return open_; }
  const std::filesystem::path& dir() const { return dir_; }
  // This process's run number in the profile (0 for the first).
  int run() const { return run_; }

  // Microseconds since the session started (on any thread).
  u64 now_us() const;
  // The same for a steady clock time (ns since its epoch).
  u64 to_us(u64 steady_ns) const;

  // The latest displayed frame, for events.
  void set_frame(u64 frame) { frame_.store(frame, std::memory_order_relaxed); }
  u64 frame() const { return frame_.load(std::memory_order_relaxed); }

  // Appends an event of type `type` (thread-safe).
  void event(std::string_view type, const Json& fields = {});
  // Appends a frame record (thread-safe).
  void frame_record(const FrameRecord& r);
  // Appends a line to system.jsonl (thread-safe).
  void system_line(const Json& fields);
  // Runs `job` on the session's writer thread (picture encoding, file writes).
  void post(std::function<void()> job);
  // Writes what is buffered to the files (also every second on its own).
  // `wait`: false to give up if another thread is writing (after a crash,
  // which may have happened while it was).
  void flush(bool wait = true);

  u64 frames_written() const { return frames_written_.load(std::memory_order_relaxed); }

 private:
  void writer_loop();
  void start_console_tee();
  void stop_console_tee();

  bool open_ = false;
  std::filesystem::path dir_;
  int run_ = 0;
  u64 origin_ns_ = 0;   // steady clock at the session's start (this process's view of it)
  std::atomic<u64> frame_{0};
  std::atomic<u64> frames_written_{0};

  std::mutex file_mutex_;  // the files below
  std::FILE* frames_ = nullptr;
  std::FILE* events_ = nullptr;
  std::FILE* system_ = nullptr;

  std::mutex job_mutex_;
  std::condition_variable job_cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::thread writer_;

  // Console output copied to console.log: stdout and stderr each go
  // through a pipe, and a thread passes what comes on to where the stream
  // went before (`saved`) and to the file.
  struct Stream {
    int fd = -1;  // 1 or 2
    int saved = -1;
    int pipe_read = -1, pipe_write = -1;
    std::thread thread;
  };
  Stream streams_[2];
  std::FILE* console_log_ = nullptr;
};

}  // namespace profile
