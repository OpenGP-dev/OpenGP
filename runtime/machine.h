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
#include "os/boot.h"
#include "os/threads.h"
#include "ppc/memory.h"
#include "ppc/module.h"
#include "triforce/io_ports.h"

namespace opengp {

using namespace gekko;

class Machine {
 public:
  Machine();
  ~Machine();

  // Folder for files the machine writes (backup memory, card saves).
  void set_user_dir(std::string dir) { user_dir_ = std::move(dir); }

  // The player's controls, as the cabinet's I/O board reads them (thread-safe).
  void set_controls(const triforce::Controls& controls) {
    std::lock_guard lock(controls_mutex_);
    controls_ = controls;
  }
  triforce::Controls controls() const {
    std::lock_guard lock(controls_mutex_);
    return controls_;
  }

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
  mutable std::mutex controls_mutex_;
  triforce::Controls controls_{};
};

}  // namespace opengp
