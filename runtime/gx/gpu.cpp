// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/gpu.h"

#include <algorithm>
#include <cstdio>

#include "os/stack_walk.h"

namespace gx {

namespace {

// CP registers.
enum : u32 {
  CP_STATUS = 0x00,
  CP_CTRL = 0x02,
  CP_CLEAR = 0x04,
  CP_PERF_SELECT = 0x06,
  CP_UNK_0A = 0x0A,
  CP_FIFO_BASE = 0x20,
  CP_FIFO_END = 0x24,
  CP_FIFO_HI_WATERMARK = 0x28,
  CP_FIFO_LO_WATERMARK = 0x2C,
  CP_FIFO_RW_DISTANCE = 0x30,
  CP_FIFO_WRITE_POINTER = 0x34,
  CP_FIFO_READ_POINTER = 0x38,
  CP_FIFO_BREAKPOINT = 0x3C,
  CP_CLKS_PER_VTX_OUT = 0x64,
};

// CP status bits.
constexpr u16 ST_OVERFLOW = 1u << 0;
constexpr u16 ST_UNDERFLOW = 1u << 1;
constexpr u16 ST_READ_IDLE = 1u << 2;
constexpr u16 ST_COMMAND_IDLE = 1u << 3;
constexpr u16 ST_BREAKPOINT = 1u << 4;

// CP control bits.
constexpr u16 CTRL_GP_READ_ENABLE = 1u << 0;
constexpr u16 CTRL_BP_ENABLE = 1u << 1;
constexpr u16 CTRL_OVERFLOW_INT = 1u << 2;
constexpr u16 CTRL_UNDERFLOW_INT = 1u << 3;
constexpr u16 CTRL_GP_LINK = 1u << 4;
constexpr u16 CTRL_BP_INT = 1u << 5;

// PE registers.
constexpr u32 PE_CTRL = 0x0A;
constexpr u32 PE_TOKEN = 0x0E;
constexpr u16 PE_TOKEN_ENABLE = 1u << 0;
constexpr u16 PE_FINISH_ENABLE = 1u << 1;
constexpr u16 PE_TOKEN_CLEAR = 1u << 2;
constexpr u16 PE_FINISH_CLEAR = 1u << 3;

constexpr u32 kFifoPointerMask = 0x03FFFFE0;

// Reads/writes the low or high half of a 32-bit register.
u16 half(u32 reg, u32 offset) { return static_cast<u16>((offset & 2) ? reg >> 16 : reg); }
void set_half(u32& reg, u32 offset, u16 v, u32 mask) {
  reg = (offset & 2) ? ((reg & 0x0000FFFF) | (u32{v} << 16)) : ((reg & 0xFFFF0000) | v);
  reg &= mask;
}

}  // namespace

class CpDevice : public hw::Device {
 public:
  CpDevice(hw::Hardware& hw, Gpu& gpu) : Device(hw), gpu_(gpu) {}
  u32 read(u32 offset, u32 size) override {
    if (size == 4) return (u32{gpu_.cp_reg_read(offset)} << 16) | gpu_.cp_reg_read(offset + 2);
    return gpu_.cp_reg_read(offset & ~1u);
  }
  void write(u32 offset, u32 value, u32 size) override {
    if (size == 4) {
      gpu_.cp_reg_write(offset, static_cast<u16>(value >> 16));
      gpu_.cp_reg_write(offset + 2, static_cast<u16>(value));
    } else {
      gpu_.cp_reg_write(offset & ~1u, static_cast<u16>(value));
    }
  }
  const char* name() const override { return "CP"; }

 private:
  Gpu& gpu_;
};

class PeDevice : public hw::Device {
 public:
  PeDevice(hw::Hardware& hw, Gpu& gpu) : Device(hw), gpu_(gpu) {}
  u32 read(u32 offset, u32 size) override {
    if (size == 4) return (u32{gpu_.pe_reg_read(offset)} << 16) | gpu_.pe_reg_read(offset + 2);
    return gpu_.pe_reg_read(offset & ~1u);
  }
  void write(u32 offset, u32 value, u32 size) override {
    if (size == 4) {
      gpu_.pe_reg_write(offset, static_cast<u16>(value >> 16));
      gpu_.pe_reg_write(offset + 2, static_cast<u16>(value));
    } else {
      gpu_.pe_reg_write(offset & ~1u, static_cast<u16>(value));
    }
  }
  const char* name() const override { return "PE"; }

 private:
  Gpu& gpu_;
};

Gpu::Gpu(hw::Hardware& hw)
    : hw_(hw), frontend_(&Gpu::read_memory, this), decoder_(*this, &Gpu::read_display_list, this) {
  frontend_.set_ram(hw.ram(0, 0), hw.mem1_size());
}
Gpu::~Gpu() = default;

std::unique_ptr<hw::Device> Gpu::make_cp_device() { return std::make_unique<CpDevice>(hw_, *this); }
std::unique_ptr<hw::Device> Gpu::make_pe_device() { return std::make_unique<PeDevice>(hw_, *this); }

const u8* Gpu::read_memory(void* context, u32 address, u32 size) {
  return static_cast<Gpu*>(context)->hw_.ram(address, size);
}

std::span<const u8> Gpu::read_display_list(void* context, u32 address, u32 size) {
  auto* gpu = static_cast<Gpu*>(context);
  const u8* p = gpu->hw_.ram(address, size);
  if (!p) {
    std::fprintf(stderr, "[gx] display list out of range: %08X size %u\n", address, size);
    gpu->frontend_.note_risk(Risk::Kind::DisplayListRange, address, size);
    return {};
  }
  return {p, size};
}

// ---- Command processor -----------------------------------------------------------------------

u32 Gpu::distance() const { return rw_distance_; }

u16 Gpu::cp_reg_read(u32 offset) {
  switch (offset & ~3u) {
    case CP_FIFO_BASE: return half(base_, offset);
    case CP_FIFO_END: return half(end_, offset);
    case CP_FIFO_HI_WATERMARK: return half(hi_watermark_, offset);
    case CP_FIFO_LO_WATERMARK: return half(lo_watermark_, offset);
    case CP_FIFO_RW_DISTANCE: return half(rw_distance_, offset);
    case CP_FIFO_WRITE_POINTER: return half(wptr_, offset);
    case CP_FIFO_READ_POINTER: return half(rptr_, offset);
    case CP_FIFO_BREAKPOINT: return half(breakpoint_, offset);
    default: break;
  }
  switch (offset) {
    case CP_STATUS: {
      u16 status = 0;
      if (rw_distance_ > hi_watermark_) status |= ST_OVERFLOW;
      if (rw_distance_ < lo_watermark_) status |= ST_UNDERFLOW;
      if (rw_distance_ == 0 || rptr_ == wptr_) status |= ST_READ_IDLE;
      if (rw_distance_ == 0 || !(cp_ctrl_ & CTRL_GP_READ_ENABLE)) status |= ST_COMMAND_IDLE;
      if ((cp_ctrl_ & CTRL_BP_ENABLE) && rptr_ == breakpoint_) status |= ST_BREAKPOINT;
      return status;
    }
    case CP_CTRL: return cp_ctrl_;
    case CP_PERF_SELECT: return perf_select_;
    case CP_UNK_0A: return unk_0a_;
    case CP_CLKS_PER_VTX_OUT: return 4;
    default: return 0;  // metrics
  }
}

void Gpu::cp_reg_write(u32 offset, u16 v) {
  switch (offset & ~3u) {
    case CP_FIFO_BASE: set_half(base_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_END: set_half(end_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_HI_WATERMARK: set_half(hi_watermark_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_LO_WATERMARK: set_half(lo_watermark_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_RW_DISTANCE:
      set_half(rw_distance_, offset, v, kFifoPointerMask);
      if (offset & 2) run();
      return;
    case CP_FIFO_WRITE_POINTER: set_half(wptr_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_READ_POINTER: set_half(rptr_, offset, v, kFifoPointerMask); return;
    case CP_FIFO_BREAKPOINT: set_half(breakpoint_, offset, v, kFifoPointerMask); return;
    default: break;
  }
  switch (offset) {
    case CP_CTRL:
      cp_ctrl_ = v & 0x3F;
      update_cp_interrupt();
      run();
      break;
    case CP_CLEAR: run(); break;  // clears overflow/underflow latches (not modelled)
    case CP_PERF_SELECT: perf_select_ = v & 7; break;
    case CP_UNK_0A: unk_0a_ = v & 0xFF; break;
    default: break;
  }
}

void Gpu::on_gather_burst() {
  // Before taking the hardware's lock: while the game waits for the
  // renderer, the machine's timers and other devices go on.
  frontend_.wait_for_output();
  // Note who writes to the pipe while a capture is coming or under way.
  const bool tracing = frontend_.capturing();
  if (tracing != tracing_) {
    tracing_ = tracing;
    hw_.gather_write_context = this;
    hw_.on_gather_write = tracing ? &Gpu::on_gather_write : nullptr;
    if (!tracing) writers_.clear();
  }
  std::lock_guard lock(hw_.mutex());
  if (cp_ctrl_ & CTRL_GP_LINK) {
    // Linked: the CPU is writing into the FIFO the GPU reads.
    wptr_ = wptr_ == end_ ? base_ : wptr_ + 32;
    rw_distance_ += 32;
  }
  run();
}

void Gpu::run() {
  if (running_) return;
  running_ = true;
  while ((cp_ctrl_ & CTRL_GP_READ_ENABLE) && rw_distance_ >= 32) {
    if ((cp_ctrl_ & CTRL_BP_ENABLE) && rptr_ == breakpoint_) break;
    const u8* src = hw_.ram(rptr_, 32);
    if (!src) {
      std::fprintf(stderr, "[gx] FIFO read pointer out of range: %08X\n", rptr_);
      rw_distance_ = 0;
      break;
    }
    const u64 burst_start = pending_start_ + pending_.size();
    pending_.insert(pending_.end(), src, src + 32);
    if (tracing_ && (cp_ctrl_ & CTRL_GP_LINK)) {
      // Linked: these are the bytes of the burst just written.
      if (bursts_.empty()) bursts_start_ = burst_start;
      bursts_.push_back(hw_.gather_burst_start());
    } else {
      bursts_.clear();
    }
    rptr_ = rptr_ == end_ ? base_ : rptr_ + 32;
    rw_distance_ -= 32;
    const std::size_t used = decoder_.run(pending_);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    pending_start_ += used;
    while (!bursts_.empty() && bursts_start_ + 32 <= pending_start_) {
      bursts_.pop_front();
      bursts_start_ += 32;
    }
    // Writers before the oldest pending byte are done with.
    const u64 oldest = bursts_.empty() ? hw_.gather_written() : bursts_.front();
    while (writers_.size() > 1 && writers_[1].position <= oldest) writers_.pop_front();
  }
  update_cp_interrupt();
  running_ = false;
}

void Gpu::update_cp_interrupt() {
  const bool breakpoint = (cp_ctrl_ & CTRL_BP_ENABLE) && (cp_ctrl_ & CTRL_BP_INT) && rptr_ == breakpoint_;
  const bool overflow = (cp_ctrl_ & CTRL_OVERFLOW_INT) && rw_distance_ > hi_watermark_;
  const bool underflow = (cp_ctrl_ & CTRL_UNDERFLOW_INT) && rw_distance_ < lo_watermark_;
  const bool asserted = (breakpoint || overflow || underflow) && (cp_ctrl_ & CTRL_GP_READ_ENABLE);
  if (asserted != cp_interrupt_) {
    cp_interrupt_ = asserted;
    hw_.set_interrupt(hw::INT_CP, asserted);
  }
}

// ---- Pixel engine ----------------------------------------------------------------------------

u16 Gpu::pe_reg_read(u32 offset) {
  switch (offset) {
    case PE_CTRL: return pe_ctrl_;
    case PE_TOKEN: return token_;
    default: return offset < pe_regs_.size() * 2 ? pe_regs_[offset / 2] : 0;
  }
}

void Gpu::pe_reg_write(u32 offset, u16 v) {
  switch (offset) {
    case PE_CTRL:
      if (v & PE_TOKEN_CLEAR) token_signaled_ = false;
      if (v & PE_FINISH_CLEAR) finish_signaled_ = false;
      pe_ctrl_ = v & (PE_TOKEN_ENABLE | PE_FINISH_ENABLE);
      update_pe_interrupts();
      break;
    case PE_TOKEN: break;  // read-only
    default:
      if (offset < pe_regs_.size() * 2) pe_regs_[offset / 2] = v;
      break;
  }
}

void Gpu::update_pe_interrupts() {
  hw_.set_interrupt(hw::INT_PE_TOKEN, token_signaled_ && (pe_ctrl_ & PE_TOKEN_ENABLE));
  hw_.set_interrupt(hw::INT_PE_FINISH, finish_signaled_ && (pe_ctrl_ & PE_FINISH_ENABLE));
}

void Gpu::signal(bool finish, bool token_interrupt, u16 token) {
  if (!finish) token_ = token;
  if (token_interrupt) token_signaled_ = true;
  if (finish) finish_signaled_ = true;
  update_pe_interrupts();
}

void Gpu::on_gather_write(void* context, u64 position) {
  auto* gpu = static_cast<Gpu*>(context);
  u64 key[6] = {};
  os::capture_stack(key, 6);
  if (!gpu->writers_.empty() && std::equal(key, key + 6, gpu->writers_.back().key)) return;
  Writer& w = gpu->writers_.emplace_back();
  w.position = position;
  std::copy(key, key + 6, w.key);
  gpu->frontend_.read_stacks(w.stacks);
}

// For captures: who wrote the first byte of the command being decoded.
void Gpu::set_issuer() {
  frontend_.set_issuer(nullptr);
  const u64 at = pending_start_ + decoder_.command_offset();
  if (bursts_.empty() || at < bursts_start_ || (at - bursts_start_) / 32 >= bursts_.size()) return;
  const u64 written = bursts_[static_cast<std::size_t>((at - bursts_start_) / 32)] + (at - bursts_start_) % 32;
  auto it = std::upper_bound(writers_.begin(), writers_.end(), written,
                             [](u64 p, const Writer& w) { return p < w.position; });
  if (it != writers_.begin()) frontend_.set_issuer(&std::prev(it)->stacks);
}

void Gpu::bp_write(u8 reg, u32 value) {
  switch (reg) {
    case BP_SET_DRAW_DONE:
      if ((value & 0xFF) == 0x02) signal(true, false, 0);
      break;
    case BP_PE_TOKEN_INT: signal(false, true, static_cast<u16>(value)); break;
    case BP_PE_TOKEN: signal(false, false, static_cast<u16>(value)); break;
    case BP_TRIGGER_EFB_COPY:
      if (value & (1u << 14)) copies_.fetch_add(1, std::memory_order_relaxed);  // copy to the XFB
      if (!bursts_.empty()) set_issuer();
      break;
    default: break;
  }
  frontend_.bp_write(reg, value);
  frontend_.set_issuer(nullptr);
}

void Gpu::draw(u8 primitive, u8 vat, u16 count, std::span<const u8> vertices) {
  // Only the thread running the game writes it: no locked add.
  draws_.store(draws_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  if (!bursts_.empty()) set_issuer();
  frontend_.draw(primitive, vat, count, vertices, decoder_.cp_regs());
  frontend_.set_issuer(nullptr);
}

}  // namespace gx
