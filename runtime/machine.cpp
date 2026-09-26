// SPDX-License-Identifier: GPL-3.0-or-later

#include "machine.h"

#include <algorithm>

#include "hw/dsp.h"
#include "hw/exi.h"
#include "hw/vi.h"

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

  // `sc` is only used (by the SDK and apploaders) to synchronise the caches:
  // the system call handler toggles HID0 and returns. Caches are not modelled.
  ppc::g_hooks.syscall = [](ppc::Context&, u8*, u32) {};

  // The disc says how much memory the game expects (Triforce titles: 48 MiB);
  // Dolphin clamps it to 24..64 MiB the same way.
  u32 mem1 = disc_.read_u32(kBi2Offset + 4);
  mem1 = mem1 ? std::clamp(mem1, kMem1Retail, kMem1Max) : kMem1Retail;
  if (!memory_.init(mem1, error)) return false;

  hw_ = std::make_unique<hw::Hardware>(memory_.base(), mem1);
  hw::Hardware& h = *hw_;
  // Devices not modelled yet keep their registers and log their first accesses.
  h.map(0x0000, 0x1000, std::make_unique<hw::RegisterFile>(h, "CP", 0x80, 32));
  h.map(0x1000, 0x1000, std::make_unique<hw::RegisterFile>(h, "PE", 0x80, 32));
  h.map(0x2000, 0x1000, std::make_unique<hw::ViDevice>(h));
  h.map(0x4000, 0x1000, std::make_unique<hw::RegisterFile>(h, "MI", 0x80, 8));
  h.map(0x5000, 0x1000, std::make_unique<hw::DspDevice>(h));
  h.map(0x6000, 0x0400, std::make_unique<hw::RegisterFile>(h, "DI", 0x40, 64));
  h.map(0x6400, 0x0400, std::make_unique<hw::RegisterFile>(h, "SI", 0x100, 64));
  h.map(0x6800, 0x0400, std::make_unique<hw::ExiBus>(h, user_dir_, disc_.game_id()));
  h.map(0x6C00, 0x0400, std::make_unique<hw::RegisterFile>(h, "AI", 0x20, 32));

  os::BootResult result;
  if (!os::boot_disc(disc_, memory_.base(), mem1, ctx_, &result, error)) return false;
  entry_ = result.entry_point;
  if (boot) *boot = result;
  return true;
}

void Machine::install(const ppc::RecompiledModule& module) { ppc::install_module(module, memory_.base()); }

void Machine::run() {
  threads_ = std::make_unique<os::ThreadManager>(*hw_, memory_.base());
  hw_->start();
  threads_->run(ctx_, entry_);
}

}  // namespace opengp
