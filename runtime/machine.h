// SPDX-License-Identifier: GPL-3.0-or-later
//
// A complete emulated Triforce around the recompiled game: guest memory, the
// hardware devices, the disc, and the guest threads. The game launcher
// creates one, boots the disc, installs the recompiled module and runs it.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "disc/disc.h"
#include "gx/gpu.h"
#include "hw/hardware.h"
#include "input/script.h"
#include "os/boot.h"
#include "os/threads.h"
#include "ppc/memory.h"
#include "ppc/module.h"
#include "triforce/card_folder.h"
#include "triforce/io_ports.h"

namespace opengp {

using namespace gekko;

class Machine {
 public:
  Machine();
  ~Machine();

  // Folder for files the machine writes (backup memory, card saves).
  void set_user_dir(std::string dir) { user_dir_ = std::move(dir); }

  // The player's save cards, in `user_dir`/cards/<game ID> (after boot()).
  triforce::CardFolder& cards() { return *cards_; }

  // The player's controls, as the cabinet's I/O board reads them (thread-safe).
  void set_controls(const triforce::Controls& controls) {
    std::lock_guard lock(controls_mutex_);
    controls_ = controls;
  }
  // What the I/O board reads: the player's controls plus the input script.
  triforce::Controls controls() const;

  // What the cabinet is doing to the player: wheel forces, lamps (thread-safe).
  triforce::Outputs outputs() const {
    std::lock_guard lock(controls_mutex_);
    return outputs_;
  }

  // Scripted input, applied on top of the player's controls (set before run()).
  void set_script(input::Script script) { script_ = std::move(script); }
  // Frames shown before this boot (by the runs the game rebooted from), for
  // the script's timing.
  void set_frame_offset(u64 frames) { frame_offset_ = frames; }
  u64 frame_offset() const { return frame_offset_; }

  // Opens the disc image, sizes RAM from its bi2.bin, maps the hardware and
  // runs the boot ROM emulation (apploader). Fills `boot` on success.
  bool boot(const std::string& image_path, os::BootResult* boot, std::string* error);

  // Registers the recompiled code (and its instruction patches).
  void install(const ppc::RecompiledModule& module);

  // Runs the game from its entry point on the calling thread. Returns only if
  // the game returns from its entry point.
  void run();

  u8* base() const { return memory_.base(); }
  u32 mem1_size() const { return memory_.mem1_size(); }
  hw::Hardware& hardware() { return *hw_; }
  gx::Gpu& gpu() { return *gpu_; }
  os::ThreadManager& threads() { return *threads_; }
  int thread_count() const { return threads_ ? threads_->thread_count() : 0; }
  u64 exceptions_delivered() const { return threads_ ? threads_->exceptions_delivered() : 0; }
  u64 exception_returns() const { return threads_ ? threads_->exception_returns() : 0; }
  const disc::Image& disc() const { return disc_; }
  ppc::Context& main_context() { return ctx_; }
  u32 entry_point() const { return entry_; }

 private:
  ppc::GuestMemory memory_;
  disc::Image disc_;
  std::unique_ptr<hw::Hardware> hw_;
  std::unique_ptr<gx::Gpu> gpu_;
  std::unique_ptr<os::ThreadManager> threads_;
  ppc::Context ctx_{};
  u32 entry_ = 0;
  std::string user_dir_ = "user";
  std::shared_ptr<triforce::CardFolder> cards_;
  mutable std::mutex controls_mutex_;
  triforce::Controls controls_{};
  triforce::Outputs outputs_{};
  input::Script script_;
  u64 frame_offset_ = 0;
};

}  // namespace opengp
