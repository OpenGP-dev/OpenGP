// SPDX-License-Identifier: GPL-3.0-or-later

#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "os/atomic_file.h"
#include "hw/ai.h"
#include "hw/di.h"
#include "hw/dsp.h"
#include "hw/exi.h"
#include "hw/si.h"
#include "hw/vi.h"
#include "triforce/baseboard.h"
#include "triforce/mediaboard.h"

namespace opengp {

namespace {

constexpr u32 kMem1Retail = 0x01800000;  // 24 MiB
constexpr u32 kMem1Max = 0x04000000;     // 64 MiB
constexpr u32 kBi2Offset = 0x440;        // bi2.bin on disc; +4 is the simulated memory size

}  // namespace

Machine::Machine() = default;

Machine::~Machine() {
  if (hw_) hw_->stop();
}

bool Machine::boot(const std::string& image_path, os::BootResult* boot, std::string* error) {
  if (!disc_.open(image_path, error)) return false;
  if (!game_id_.empty() && disc_.game_id() != game_id_) disc_.set_game_id(game_id_);
  disc_.preload();

  // `sc` is only used (by the SDK and apploaders) to synchronise the caches:
  // the system call handler toggles HID0 and returns. Caches are not modelled.
  ppc::g_hooks.syscall = [](ppc::Context&, u8*, u32) {};

  // The disc says how much memory the game expects (GP 2: 48 MiB, GP: 24 MiB);
  // Dolphin clamps it to 24..64 MiB the same way.
  u32 mem1 = disc_.read_u32(kBi2Offset + 4);
  mem1 = mem1 ? std::clamp(mem1, kMem1Retail, kMem1Max) : kMem1Retail;
  if (!memory_.init(mem1, error)) return false;

  hw_ = std::make_unique<hw::Hardware>(memory_.base(), mem1);
  hw::Hardware& h = *hw_;
  h.set_timebase(os::boot_time_base());
  // Devices not modelled yet keep their registers and log their first accesses.
  gpu_ = std::make_unique<gx::Gpu>(h);
  h.map(0x0000, 0x1000, gpu_->make_cp_device());
  h.map(0x1000, 0x1000, gpu_->make_pe_device());
  h.on_gather_burst = [gpu = gpu_.get()] { gpu->on_gather_burst(); };
  h.on_efb_read = [gpu = gpu_.get()](u32 addr) { return gpu->efb_read(addr); };
  h.on_efb_write = [gpu = gpu_.get()](u32 addr, u32 value) { gpu->efb_write(addr, value); };
  h.map(0x2000, 0x1000, std::make_unique<hw::ViDevice>(h));
  h.map(0x4000, 0x1000, std::make_unique<hw::RegisterFile>(h, "MI", 0x80, 8));
  h.map(0x5000, 0x1000, std::make_unique<hw::DspDevice>(h));
  // Triforce: the media board answers the disc interface.
  auto di = std::make_unique<hw::DiDevice>(h, disc_, true);
  auto mediaboard = std::make_unique<triforce::MediaBoard>(h, disc_, user_dir_);
  triforce::MediaBoard* mb = mediaboard.get();
  network_ = &mb->network();
  di->set_backend(std::move(mediaboard));
  h.map(0x6000, 0x0400, std::move(di));
  // Save cards. Before cards had a folder, the one card was tricard_<ID>.bin.
  cards_ = std::make_shared<triforce::CardFolder>(user_dir_ + "/cards/" + disc_.game_id());
  {
    namespace fs = std::filesystem;
    const fs::path legacy = os::utf8_path(user_dir_ + "/tricard_" + disc_.game_id() + ".bin");
    std::error_code ec;
    if (fs::is_regular_file(legacy, ec) && cards_->list().empty()) {
      fs::create_directories(os::utf8_path(cards_->dir()), ec);
      fs::rename(legacy, os::utf8_path(cards_->path("Card 1")), ec);
      if (!ec) std::fprintf(stderr, "[card] moved %s to %s\n", os::utf8(legacy).c_str(), cards_->path("Card 1").c_str());
    }
  }
  auto si = std::make_unique<hw::SiBus>(h);
  si->attach(0, std::make_unique<triforce::BaseboardSi>(
                   disc_.game_id(), cards_, [this] { return controls(); },
                   [this](const triforce::Outputs& outputs) {
                     std::lock_guard lock(controls_mutex_);
                     outputs_ = outputs;
                   }));
  h.map(0x6400, 0x0400, std::move(si));
  h.map(0x6800, 0x0400, std::make_unique<hw::ExiBus>(h, user_dir_, disc_.game_id()));
  h.map(0x6C00, 0x0400, std::make_unique<hw::AiDevice>(h));

  os::BootResult result;
  if (!os::boot_disc(disc_, memory_.base(), mem1, ctx_, &result, error)) return false;
  if (const u8* low = h.ram(0x80000000, 12)) {
    auto be32 = [](const u8* p) { return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3]; };
    mb->init_keys(be32(low), be32(low + 4), be32(low + 8));
  }
  entry_ = result.entry_point;
  if (boot) *boot = result;
  return true;
}

triforce::Controls Machine::controls() const {
  triforce::Controls c;
  {
    std::lock_guard lock(controls_mutex_);
    c = controls_;
  }
  if (!script_.empty() && gpu_) script_.apply(frame_offset_ + gpu_->frames(), c);
  return c;
}

void Machine::install(const ppc::RecompiledModule& module) { ppc::install_module(module, memory_.base()); }

void Machine::run() {
  threads_ = std::make_unique<os::ThreadManager>(*hw_, memory_.base());
  hw_->start();
  threads_->run(ctx_, entry_);
}

}  // namespace opengp
