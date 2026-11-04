// SPDX-License-Identifier: GPL-3.0-or-later
//
// Register semantics follow Dolphin (Source/Core/Core/HW/*), Copyright
// Dolphin Emulator Project, GPL-2.0-or-later.

#include "hw/hardware.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "os/thread_priority.h"
#include "os/verbose.h"
#include "ppc/hooks.h"
#include "ppc/memory.h"
#include "ppc/trace.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace hw {

namespace {

constexpr u32 kIoBase = 0xCC000000;
constexpr u32 kPhysicalMask = 0x03FFFFFF;  // GameCube physical address space (64 MiB)
constexpr u32 kFlipperRevC = 0x246500B1;

Hardware* g_hw = nullptr;

using ppc::ns_to_ticks;
using ppc::ticks_to_ns;

}  // namespace

// ---- PI: interrupt controller and CPU FIFO -------------------------------------------------

class PiDevice : public Device {
 public:
  using Device::Device;
  const char* name() const override { return "PI"; }

  u32 read(u32 offset, u32 size) override {
    const u32 v = read32(offset & ~3u);
    if (size == 4) return v;
    return (offset & 2) ? (v & 0xFFFF) : (v >> 16);
  }

  void write(u32 offset, u32 value, u32 size) override {
    if (size != 4) {  // merge 16-bit writes into the 32-bit register
      const u32 old = read32(offset & ~3u);
      value = (offset & 2) ? ((old & 0xFFFF0000) | (value & 0xFFFF)) : ((old & 0xFFFF) | (value << 16));
    }
    write32(offset & ~3u, value);
  }

 private:
  u32 read32(u32 offset) {
    switch (offset) {
      case 0x00: return hw_.pi_cause_;
      case 0x04: return hw_.pi_mask_;
      case 0x0C: return hw_.fifo_base_;
      case 0x10: return hw_.fifo_end_;
      case 0x14: return hw_.fifo_wptr_;
      case 0x1C: return error_cause_;
      case 0x20: return error_address_;
      case 0x24: return reset_code_;
      case 0x28: return unknown_;
      case 0x2C: return kFlipperRevC;
      case 0x30: return bus_strength_;
      default: return 0;
    }
  }

  void write32(u32 offset, u32 v) {
    switch (offset) {
      case 0x00:  // writing 1s acknowledges latched causes
        hw_.pi_cause_ &= ~v;
        hw_.update_pending();
        break;
      case 0x04:
        hw_.pi_mask_ = v;
        hw_.update_pending();
        break;
      case 0x0C: hw_.fifo_base_ = v & kPhysicalMask & 0xFFFFFFE0; break;
      case 0x10: hw_.fifo_end_ = v & kPhysicalMask & 0xFFFFFFE0; break;
      case 0x14:
        hw_.fifo_wptr_ = v & kPhysicalMask & 0xFFFFFFE0;
        hw_.gather_count_ = 0;
        break;
      case 0x18: hw_.gather_count_ = 0; break;  // FIFO reset (GXAbortFrame)
      case 0x1C: error_cause_ = v & 7; break;
      case 0x20: error_address_ = v; break;
      case 0x24: reset_code_ = v; break;
      case 0x28: unknown_ = v & 0x3FF; break;
      case 0x30: bus_strength_ = v & 0x07FFFFFF; break;
      default: break;
    }
  }

  u32 error_cause_ = 0, error_address_ = 0, reset_code_ = 0, unknown_ = 0x1FF, bus_strength_ = 0x02492492;
};

// ---- RegisterFile ----------------------------------------------------------------------------

void RegisterFile::log(const char* kind, u32 offset, u32 value, u32 size) {
  if (logged_ >= log_limit_ || !os::verbose()) return;
  ++logged_;
  // With call tracing enabled, name the guest function making the access.
  char caller[32] = "";
  const auto frames = ppc::trace_snapshot(ppc::trace_stack());
  if (!frames.empty()) std::snprintf(caller, sizeof(caller), " (in %08X)", frames.back());
  std::fprintf(stderr, "[%s] %s%u +%03X = %08X%s%s\n", name_, kind, size * 8, offset, value, caller,
               logged_ == log_limit_ ? " (further accesses not shown)" : "");
}

u32 RegisterFile::read(u32 offset, u32 size) {
  u32 v;
  if (size == 4) v = reg32(offset);
  else if (size == 2) v = reg16(offset);
  else v = (reg16(offset & ~1u) >> ((offset & 1) ? 0 : 8)) & 0xFF;
  log("read", offset, v, size);
  return v;
}

void RegisterFile::write(u32 offset, u32 value, u32 size) {
  log("write", offset, value, size);
  if (size == 4) {
    set_reg32(offset, value);
  } else if (size == 2) {
    set_reg16(offset, static_cast<u16>(value));
  } else {
    u16 r = reg16(offset & ~1u);
    r = (offset & 1) ? static_cast<u16>((r & 0xFF00) | (value & 0xFF)) : static_cast<u16>((r & 0x00FF) | ((value & 0xFF) << 8));
    set_reg16(offset & ~1u, r);
  }
}

// ---- Hardware --------------------------------------------------------------------------------

Hardware::Hardware(u8* base, u32 mem1_size) : base_(base), mem1_size_(mem1_size) {
  tb_origin_ = host_ticks();
  map(0x3000, 0x100, std::make_unique<PiDevice>(*this));
}

Hardware::~Hardware() {
  stop();
#ifdef _WIN32
  if (timer_wake_) CloseHandle(timer_wake_);
#endif
}

void Hardware::map(u32 block_base, u32 size, std::unique_ptr<Device> device) {
  mappings_.push_back({block_base, size, device.get()});
  devices_.push_back(std::move(device));
}

u8* Hardware::ram(u32 addr, u32 size) const {
  const u32 phys = addr & 0x1FFFFFFF;
  if (u64{phys} + size > mem1_size_) return nullptr;
  return base_ + (ppc::MEM1_CACHED | phys);
}

namespace {

u64 steady_now_ns() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace

Hardware::ClockMap Hardware::clock() const {
  for (;;) {
    const u32 seq = clock_seq_.load(std::memory_order_acquire);
    if (seq & 1) continue;
    ClockMap c;
    c.anchor_ns = clock_anchor_ns_.load(std::memory_order_relaxed);
    c.anchor_ticks = clock_anchor_ticks_.load(std::memory_order_relaxed);
    c.rate = clock_rate_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (clock_seq_.load(std::memory_order_relaxed) == seq) return c;
  }
}

u64 Hardware::ticks_at(u64 steady_ns) const {
  const ClockMap c = clock();
  if (c.rate == 1.0) return ns_to_ticks(steady_ns) - ns_to_ticks(c.anchor_ns) + c.anchor_ticks;
  if (steady_ns >= c.anchor_ns)
    return c.anchor_ticks + ns_to_ticks(static_cast<u64>(std::llround(static_cast<double>(steady_ns - c.anchor_ns) * c.rate)));
  return c.anchor_ticks - ns_to_ticks(static_cast<u64>(std::llround(static_cast<double>(c.anchor_ns - steady_ns) * c.rate)));
}

u64 Hardware::ns_at_ticks(u64 ticks) const {
  const ClockMap c = clock();
  u64 ns;
  if (c.rate == 1.0) {
    ns = ppc::ticks_to_ns_up(ticks - c.anchor_ticks + ns_to_ticks(c.anchor_ns));
  } else if (ticks >= c.anchor_ticks) {
    ns = c.anchor_ns + static_cast<u64>(std::ceil(static_cast<double>(ppc::ticks_to_ns_up(ticks - c.anchor_ticks)) / c.rate));
  } else {
    ns = c.anchor_ns - static_cast<u64>(static_cast<double>(ppc::ticks_to_ns(c.anchor_ticks - ticks)) / c.rate);
  }
  // Rounding: the first nanosecond whose ticks reach `ticks`.
  while (ticks_at(ns) < ticks) ++ns;
  while (ns > 0 && ticks_at(ns - 1) >= ticks) --ns;
  return ns;
}

u64 Hardware::host_ticks() const { return ticks_at(steady_now_ns()); }

void Hardware::set_clock_rate(double rate) {
  if (!(rate > 0.9 && rate < 1.1)) rate = 1.0;
  std::lock_guard lock(mutex_);
  const u64 now = steady_now_ns();
  const u64 ticks = ticks_at(now);
  clock_seq_.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  clock_anchor_ns_.store(now, std::memory_order_relaxed);
  clock_anchor_ticks_.store(ticks, std::memory_order_relaxed);
  clock_rate_.store(rate, std::memory_order_relaxed);
  clock_seq_.fetch_add(1, std::memory_order_release);
  wake_timer();  // timed events are due at other host times now
}

u64 Hardware::clock_ticks() const {
  const u64 paused = paused_at_.load(std::memory_order_acquire);
  return paused ? paused : host_ticks();
}

u64 Hardware::timebase() const { return clock_ticks() - tb_origin_; }

u64 Hardware::timebase_at(u64 steady_ns) const { return ticks_at(steady_ns) - tb_origin_; }

// Rounded up, so that timebase_at(steady_ns_at(tb)) == tb.
u64 Hardware::steady_ns_at(u64 timebase) const {
  return ns_at_ticks(timebase + tb_origin_);
}

void Hardware::set_timebase(u64 value) {
  std::lock_guard lock(mutex_);
  tb_origin_ = clock_ticks() - value;
}

void Hardware::set_paused(bool paused) {
  std::lock_guard lock(mutex_);
  if (paused == this->paused()) return;
  if (paused) {
    paused_at_.store(std::max<u64>(host_ticks(), 1), std::memory_order_release);
  } else {
    tb_origin_ += host_ticks() - paused_at_.load(std::memory_order_relaxed);
    paused_at_.store(0, std::memory_order_release);
    pause_cv_.notify_all();
  }
  update_pending();  // while paused, guest code goes to its next safe point
  wake_timer();
}

void Hardware::wake_timer() {
  timer_cv_.notify_all();
#ifdef _WIN32
  if (timer_wake_) SetEvent(timer_wake_);
#endif
}

void Hardware::wait_while_paused() {
  if (!paused()) return;
  std::unique_lock lock(mutex_);
  pause_cv_.wait(lock, [this] { return !paused(); });
}

u32 Hardware::decrementer() const {
  const u64 elapsed = timebase() - dec_written_;
  return dec_value_ - static_cast<u32>(elapsed);
}

void Hardware::set_decrementer(u32 value) {
  std::lock_guard lock(mutex_);
  dec_value_ = value;
  dec_written_ = timebase();
  if (dec_event_) cancel(dec_event_);
  dec_event_ = 0;
  // The exception fires when the decrementer passes from 0 to -1.
  if (!(value & 0x80000000)) {
    dec_event_ = schedule_at(dec_written_ + value + 1, [this] {
      dec_event_ = 0;
      dec_pending_ = true;
      update_pending();
    });
  }
}

void Hardware::set_interrupt(u32 cause, bool asserted) {
  std::lock_guard lock(mutex_);
  if (asserted) pi_cause_ |= cause;
  else pi_cause_ &= ~cause;
  update_pending();
}

void Hardware::update_pending() {
  const bool pending = paused() || dec_pending_ || (pi_cause_ & pi_mask_) != 0;
  ppc::g_interrupt_pending.store(pending, std::memory_order_release);
  if (pending) idle_cv_.notify_all();
}

Exception Hardware::take_exception() {
  std::lock_guard lock(mutex_);
  Exception e = Exception::None;
  if (pi_cause_ & pi_mask_) {
    e = Exception::External;  // higher priority than the decrementer
  } else if (dec_pending_) {
    dec_pending_ = false;
    e = Exception::Decrementer;
  }
  update_pending();
  return e;
}

void Hardware::wait_for_interrupt(u32 timeout_ms) {
  std::unique_lock lock(mutex_);
  idle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                    [] { return ppc::g_interrupt_pending.load(std::memory_order_acquire); });
}

Hardware::EventId Hardware::schedule_at(u64 when, std::function<void()> fn) {
  std::lock_guard lock(mutex_);
  const EventId id = next_event_id_++;
  const auto it = events_.emplace(when, std::make_pair(id, std::move(fn)));
  // The timer thread sleeps until the earliest event: only a new earliest one
  // needs to wake it.
  if (it == events_.begin()) wake_timer();
  return id;
}

void Hardware::cancel(EventId id) {
  std::lock_guard lock(mutex_);
  for (auto it = events_.begin(); it != events_.end(); ++it) {
    if (it->second.first == id) {
      events_.erase(it);
      return;
    }
  }
}

void Hardware::timer_loop() {
  os::set_interactive_thread();
  os::set_thread_name("timer");
#ifdef _WIN32
  // A condition variable's timeout is whole milliseconds on Windows (MSVC's
  // library rounds it up, so events came 1-2 ms late; libc++'s rounds it down,
  // so the last millisecond before each event spun, taking the lock each
  // time). A high-resolution waitable timer sleeps to the event (Windows 10
  // 1803 and later; a plain one before).
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
#endif
  std::unique_lock lock(mutex_);
  while (running_) {
    if (events_.empty() || paused()) {
      timer_cv_.wait(lock);
      continue;
    }
    const u64 now = timebase();
    auto it = events_.begin();
    if (it->first > now) {
      const u64 ns = ppc::ticks_to_ns_up(it->first - now);  // never before the event
#ifdef _WIN32
      if (timer && timer_wake_) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>((ns + 99) / 100));  // relative, 100 ns units
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        lock.unlock();
        HANDLE handles[2] = {timer, static_cast<HANDLE>(timer_wake_)};
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        lock.lock();
        continue;
      }
#endif
      timer_cv_.wait_for(lock, std::chrono::nanoseconds(ns));
      continue;
    }
    auto fn = std::move(it->second.second);
    events_.erase(it);
    fn();  // runs with the mutex held; may schedule more events
  }
#ifdef _WIN32
  if (timer) CloseHandle(timer);
#endif
}

void Hardware::start() {
  g_hw = this;
  ppc::g_hooks.mmio_read = [](u32 addr, u32 size) { return g_hw->mmio_read(addr, size); };
  ppc::g_hooks.mmio_write = [](u32 addr, u32 value, u32 size) { g_hw->mmio_write(addr, value, size); };
  ppc::g_hooks.gather_pipe_write = [](const u8* bytes, u32 size) { g_hw->gather_write(bytes, size); };
  ppc::g_hooks.read_timebase = [] { return g_hw->timebase(); };
  ppc::g_hooks.write_timebase = [](u64 v) { g_hw->set_timebase(v); };
  ppc::g_hooks.read_decrementer = [] { return g_hw->decrementer(); };
  ppc::g_hooks.write_decrementer = [](u32 v) { g_hw->set_decrementer(v); };
#ifdef _WIN32
  timeBeginPeriod(1);  // 1 ms timer resolution for the event thread
#endif
  running_ = true;
#ifdef _WIN32
  if (!timer_wake_) timer_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
#endif
  timer_thread_ = std::thread([this] { timer_loop(); });
}

void Hardware::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!running_) return;
    running_ = false;
    wake_timer();
  }
  if (timer_thread_.joinable()) timer_thread_.join();
#ifdef _WIN32
  timeEndPeriod(1);
#endif
  if (g_hw == this) g_hw = nullptr;
}

u32 Hardware::mmio_read(u32 addr, u32 size) {
  // The CPU's window onto the EFB is served by the GPU front end, whose state
  // only the thread running guest code touches, without the device lock: a
  // peek may wait for the renderer (on the main thread), which itself needs
  // the lock now and then (to pause, for the next retrace's time).
  if ((addr >> 24) == 0xC8) {
    const u32 v = on_efb_read ? on_efb_read(addr & ~3u) : 0;
    if (size == 4) return v;
    if (size == 2) return (v >> ((addr & 2) ? 0 : 16)) & 0xFFFF;
    return (v >> (8 * (3 - (addr & 3)))) & 0xFF;
  }
  std::lock_guard lock(mutex_);
  const u32 off = addr - kIoBase;
  for (const Mapping& m : mappings_)
    if (off >= m.base && off < m.base + m.size) return m.device->read(off - m.base, size);
  if (os::verbose()) std::fprintf(stderr, "[hw] unmapped read%u %08X\n", size * 8, addr);
  return 0;
}

void Hardware::mmio_write(u32 addr, u32 value, u32 size) {
  if ((addr >> 24) == 0xC8) {  // the EFB window: see mmio_read
    if (on_efb_write) on_efb_write(addr & ~3u, value);
    return;
  }
  std::lock_guard lock(mutex_);
  const u32 off = addr - kIoBase;
  for (const Mapping& m : mappings_) {
    if (off >= m.base && off < m.base + m.size) {
      m.device->write(off - m.base, value, size);
      return;
    }
  }
  if (os::verbose()) std::fprintf(stderr, "[hw] unmapped write%u %08X = %08X\n", size * 8, addr, value);
}

// The gather pipe is only ever written by the thread running guest code, and
// its state is only touched from that thread, so it needs no lock.
void Hardware::gather_write(const u8* bytes, u32 size) {
  if (on_gather_write) [[unlikely]]
    on_gather_write(gather_write_context, gather_written_);
  std::memcpy(gather_buffer_.data() + gather_count_, bytes, size);
  gather_count_ += size;
  gather_written_ += size;
  if (gather_count_ < 32) return;
  gather_burst_start_ = gather_written_ - gather_count_;
  u8* dst = ram(fifo_wptr_, 32);
  if (dst) std::memcpy(dst, gather_buffer_.data(), 32);
  if (fifo_wptr_ == fifo_end_) fifo_wptr_ = fifo_base_;
  else fifo_wptr_ += 32;
  gather_count_ -= 32;
  std::memmove(gather_buffer_.data(), gather_buffer_.data() + 32, gather_count_);
  if (on_gather_burst) on_gather_burst();
}

}  // namespace hw
