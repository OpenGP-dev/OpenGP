// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GameCube/Triforce hardware around the CPU: memory-mapped registers at
// 0xCC000000, the interrupt controller (PI), the time base and decrementer,
// the write-gather pipe, and a timer thread for timed hardware events.
//
// Register semantics follow Dolphin (Source/Core/Core/HW/*), Copyright
// Dolphin Emulator Project, GPL-2.0-or-later.
//
// Threading: guest code runs on one host thread at a time (see os/threads.h)
// and reaches the hardware through MMIO hooks; timed events run on the timer
// thread. Both take Hardware::mutex(), which guards all device state.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ppc/context.h"
#include "ppc/timebase.h"

namespace hw {

// The clock rate a linked game asks for, to stay in step with the host
// (games/common/link_pace.h); the launcher applies it during a linked race.
inline std::atomic<double> g_link_clock_rate{1.0};

using namespace gekko;

// PI interrupt cause bits (INTSR / INTMR).
enum InterruptCause : u32 {
  INT_PI = 0x1,  // GP runtime error
  INT_RSW = 0x2,  // reset switch
  INT_DI = 0x4,
  INT_SI = 0x8,
  INT_EXI = 0x10,
  INT_AI = 0x20,
  INT_DSP = 0x40,
  INT_MEM = 0x80,
  INT_VI = 0x100,
  INT_PE_TOKEN = 0x200,
  INT_PE_FINISH = 0x400,
  INT_CP = 0x800,
  INT_DEBUG = 0x1000,
  INT_HSP = 0x2000,
  INT_RSW_STATE = 0x10000,  // reset switch state: set while not pressed
};

// PowerPC exceptions the hardware can raise at the CPU.
enum class Exception : u32 { None = 0, External = 4, Decrementer = 8 };

inline constexpr u64 kTimebaseHz = ppc::kTimebaseHz;  // bus clock (162 MHz) / 4
inline constexpr u32 kBusClockHz = 162000000;

class Hardware;

// A block of memory-mapped registers. Offsets are relative to the block base.
class Device {
 public:
  explicit Device(Hardware& hw) : hw_(hw) {}
  virtual ~Device() = default;
  virtual u32 read(u32 offset, u32 size) = 0;
  virtual void write(u32 offset, u32 value, u32 size) = 0;
  virtual const char* name() const = 0;

 protected:
  Hardware& hw_;
};

class Hardware {
 public:
  Hardware(u8* base, u32 mem1_size);
  ~Hardware();
  Hardware(const Hardware&) = delete;
  Hardware& operator=(const Hardware&) = delete;

  // Maps a device over [block_base, block_base + size) of the 0xCC000000 space.
  void map(u32 block_base, u32 size, std::unique_ptr<Device> device);
  template <typename T>
  T* device() {
    for (auto& d : devices_)
      if (auto* p = dynamic_cast<T*>(d.get())) return p;
    return nullptr;
  }

  // Installs the ppc::g_hooks entry points (MMIO, gather pipe, time base,
  // decrementer) and starts the timer thread.
  void start();
  void stop();

  u8* base() const { return base_; }
  u32 mem1_size() const { return mem1_size_; }
  std::recursive_mutex& mutex() { return mutex_; }

  // Guest physical/virtual address -> host pointer into MEM1 (or nullptr).
  u8* ram(u32 addr, u32 size = 1) const;

  // --- Interrupts ---------------------------------------------------------------------
  void set_interrupt(u32 cause, bool asserted);
  u32 interrupt_cause() const { return pi_cause_; }
  u32 interrupt_mask() const { return pi_mask_; }
  // The exception to deliver now (ignores MSR[EE]); acknowledges the decrementer.
  Exception take_exception();
  // Blocks until an exception may be pending or `timeout_ms` passes (idle loop).
  void wait_for_interrupt(u32 timeout_ms);

  // --- Time -----------------------------------------------------------------------------
  u64 timebase() const;  // ticks since boot (40.5 MHz), plus any guest adjustment
  void set_timebase(u64 value);
  // The time base at a time of the host's steady clock (std::chrono::
  // steady_clock, in nanoseconds), and back (the first nanosecond at or
  // after it), while the machine runs.
  u64 timebase_at(u64 steady_ns) const;
  u64 steady_ns_at(u64 timebase) const;
  u32 decrementer() const;
  void set_decrementer(u32 value);
  // How fast the machine's clock runs against the host's: 1 is real time.
  // The launcher sets it a little above 1 (0.1%) so that the game's 59.94 Hz
  // retraces line up with a 60 Hz display's refreshes (a display frame would
  // otherwise be shown twice every 16.7 seconds). The time base carries on
  // from where it is: no jump. Thread-safe.
  void set_clock_rate(double rate);
  double clock_rate() const { return clock_rate_.load(std::memory_order_relaxed); }

  // --- Pause --------------------------------------------------------------------------
  // Freezes the machine: the time base and decrementer stop, timed events
  // (retraces, DMA, the audio) wait, and guest code stops at its next safe
  // point, in wait_while_paused(). Resuming carries on as if no time had
  // passed. Thread-safe.
  void set_paused(bool paused);
  bool paused() const { return paused_at_.load(std::memory_order_acquire) != 0; }
  // Blocks the calling (guest) thread while the machine is paused.
  void wait_while_paused();

  // --- Timed events -------------------------------------------------------------------
  using EventId = u64;
  // Runs `fn` on the timer thread (with mutex() held) at time base `when`.
  EventId schedule_at(u64 when, std::function<void()> fn);
  EventId schedule_after(u64 ticks, std::function<void()> fn) { return schedule_at(timebase() + ticks, std::move(fn)); }
  void cancel(EventId id);

  // --- Write-gather pipe --------------------------------------------------------------
  // CPU FIFO registers (PI_FIFO_*). Bursts are written to RAM at the write pointer.
  u32 fifo_base() const { return fifo_base_; }
  u32 fifo_end() const { return fifo_end_; }
  u32 fifo_write_pointer() const { return fifo_wptr_; }
  // Called after each 32-byte burst lands in RAM (the GPU hooks this).
  std::function<void()> on_gather_burst;

  // CPU access to the EFB window (0xC8000000-0xC8FFFFFF; the GPU hooks these).
  std::function<u32(u32 addr)> on_efb_read;
  std::function<void(u32 addr, u32 value)> on_efb_write;

 private:
  friend class PiDevice;
  u32 mmio_read(u32 addr, u32 size);
  void mmio_write(u32 addr, u32 value, u32 size);
  void gather_write(const u8* bytes, u32 size);
  void update_pending();
  void timer_loop();
  u64 host_ticks() const;
  u64 clock_ticks() const;  // host_ticks(), or when it stopped while paused
  // The machine's clock in host ticks at a time of the steady clock, and back.
  u64 ticks_at(u64 steady_ns) const;
  u64 ns_at_ticks(u64 ticks) const;  // the first nanosecond at or after

  u8* base_;
  u32 mem1_size_;
  std::recursive_mutex mutex_;

  struct Mapping {
    u32 base, size;
    Device* device;
  };
  std::vector<std::unique_ptr<Device>> devices_;
  std::vector<Mapping> mappings_;

  // PI
  u32 pi_cause_ = INT_RSW_STATE | INT_VI;
  u32 pi_mask_ = 0;
  u32 fifo_base_ = 0, fifo_end_ = 0, fifo_wptr_ = 0;
  std::array<u8, 64> gather_buffer_{};  // one 32-byte burst plus spill from up to 8-byte writes
  u32 gather_count_ = 0;

  // Time base: tb = host_ticks - tb_origin_ (+ guest adjustments, and time
  // spent paused)
  u64 tb_origin_ = 0;
  // The clock's rate: from the steady clock's anchor_ns on, it runs at
  // `rate`, starting from anchor_ticks. Read without a lock.
  struct ClockMap {
    u64 anchor_ns = 0;
    u64 anchor_ticks = 0;
    double rate = 1.0;
  };
  ClockMap clock() const;
  std::atomic<u32> clock_seq_{0};  // odd while set_clock_rate() changes the three below
  std::atomic<u64> clock_anchor_ns_{0}, clock_anchor_ticks_{0};
  std::atomic<double> clock_rate_{1.0};
  std::atomic<u64> paused_at_{0};  // host ticks when paused; 0 when running
  std::condition_variable_any pause_cv_;
  // Decrementer: value `dec_value_` written at time base `dec_written_`.
  u32 dec_value_ = 0;
  u64 dec_written_ = 0;
  bool dec_pending_ = false;
  EventId dec_event_ = 0;

  // Timer thread
  std::thread timer_thread_;
  std::condition_variable_any timer_cv_;
  void wake_timer();  // after the earliest event, the pause or running_ changed
#ifdef _WIN32
  void* timer_wake_ = nullptr;  // an event: the timer thread waits on it and a high-resolution timer
#endif
  std::condition_variable_any idle_cv_;
  bool running_ = false;
  EventId next_event_id_ = 1;
  std::multimap<u64, std::pair<EventId, std::function<void()>>> events_;
};

// Hardware registers that only need storage (and logging of unknown accesses).
class RegisterFile : public Device {
 public:
  // `log_limit`: how many accesses to print (for finding out what a device
  // that is not modelled yet is used for).
  RegisterFile(Hardware& hw, const char* name, u32 size, u32 log_limit = 0)
      : Device(hw), name_(name), regs_(size / 2, 0), log_limit_(log_limit) {}
  u32 read(u32 offset, u32 size) override;
  void write(u32 offset, u32 value, u32 size) override;
  const char* name() const override { return name_; }

 protected:
  u16 reg16(u32 offset) const { return regs_[(offset / 2) % regs_.size()]; }
  u32 reg32(u32 offset) const { return (u32{reg16(offset)} << 16) | reg16(offset + 2); }
  void set_reg16(u32 offset, u16 v) { regs_[(offset / 2) % regs_.size()] = v; }
  void set_reg32(u32 offset, u32 v) {
    set_reg16(offset, static_cast<u16>(v >> 16));
    set_reg16(offset + 2, static_cast<u16>(v));
  }

 private:
  void log(const char* kind, u32 offset, u32 value, u32 size);
  const char* name_;
  std::vector<u16> regs_;
  u32 log_limit_;
  u32 logged_ = 0;
};

}  // namespace hw
