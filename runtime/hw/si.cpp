// SPDX-License-Identifier: GPL-3.0-or-later

#include "hw/si.h"

#include <cstdio>
#include <cstdlib>

namespace hw {

namespace {

// Register offsets.
constexpr u32 SI_POLL = 0x30;
constexpr u32 SI_COM_CSR = 0x34;
constexpr u32 SI_STATUS = 0x38;
constexpr u32 SI_EXI_CLOCK_COUNT = 0x3C;
constexpr u32 SI_IO_BUFFER = 0x80;

// Communication control/status register.
constexpr u32 CSR_TSTART = 1u << 0;
constexpr u32 CSR_CHANNEL = 3u << 1;
constexpr u32 CSR_INLNGTH = 0x7Fu << 8;
constexpr u32 CSR_OUTLNGTH = 0x7Fu << 16;
constexpr u32 CSR_RDSTINTMSK = 1u << 27;
constexpr u32 CSR_RDSTINT = 1u << 28;
constexpr u32 CSR_COMERR = 1u << 29;
constexpr u32 CSR_TCINTMSK = 1u << 30;
constexpr u32 CSR_TCINT = 1u << 31;

// Status register: one byte per channel, channel 0 in the top byte.
constexpr u32 ST_ERRORS = 0x0F0F0F0F;  // UNRUN, OVRUN, COLL, NOREP (write 1 to clear)
constexpr u32 ST_WRST = 0x10101010;
constexpr u32 ST_RDST = 0x20202020;
constexpr u32 ST_WR = 1u << 31;
constexpr u32 st_norep(int channel) { return 0x08000000u >> (channel * 8); }
constexpr u32 st_rdst(int channel) { return 0x20000000u >> (channel * 8); }

// Transfer lengths: 0 means 128.
constexpr int transfer_length(u32 field) { return static_cast<int>(((field - 1) & 0x7F) + 1); }

}  // namespace

SiBus::SiBus(Hardware& hw) : Device(hw) {
  if (const char* env = std::getenv("OPENGP_LOG_SI")) log_remaining_ = std::atoi(env) > 1 ? std::atoi(env) : 200;
  for (Channel& c : channels_) c.device = std::make_unique<SiNullDevice>();
}

void SiBus::attach(int channel, std::unique_ptr<SiDevice> device) {
  channels_.at(channel).device = device ? std::move(device) : std::make_unique<SiNullDevice>();
}

u32 SiBus::read(u32 offset, u32 size) {
  if (offset >= SI_IO_BUFFER) {
    const u32 i = (offset - SI_IO_BUFFER) % kBufferSize;
    u32 v = 0;
    for (u32 b = 0; b < size && i + b < kBufferSize; ++b) v = (v << 8) | buffer_[i + b];
    return v;
  }
  if (size != 4) return 0;
  if (offset < SI_POLL) {
    Channel& c = channels_[offset / 0xC];
    switch (offset % 0xC) {
      case 0x0: return c.out;
      case 0x4:
      case 0x8:
        // Reading the input registers acknowledges the new data.
        status_ &= ~st_rdst(static_cast<int>(offset / 0xC));
        update_interrupts();
        return offset % 0xC == 0x4 ? c.in_hi : c.in_lo;
    }
  }
  switch (offset) {
    case SI_POLL: return poll_;
    case SI_COM_CSR: return com_csr_;
    case SI_STATUS: return status_;
    case SI_EXI_CLOCK_COUNT: return exi_clock_count_;
    default: return 0;
  }
}

void SiBus::write(u32 offset, u32 v, u32 size) {
  if (offset >= SI_IO_BUFFER) {
    const u32 i = (offset - SI_IO_BUFFER) % kBufferSize;
    for (u32 b = 0; b < size && i + b < kBufferSize; ++b) buffer_[i + b] = static_cast<u8>(v >> (8 * (size - 1 - b)));
    return;
  }
  if (size != 4) return;
  if (offset < SI_POLL) {
    Channel& c = channels_[offset / 0xC];
    switch (offset % 0xC) {
      case 0x0: c.out = v; break;
      case 0x4: c.in_hi = v; break;
      case 0x8: c.in_lo = v; break;
    }
    return;
  }
  switch (offset) {
    case SI_POLL: poll_ = v; break;
    case SI_COM_CSR: {
      constexpr u32 kWritable = CSR_CHANNEL | CSR_INLNGTH | CSR_OUTLNGTH | CSR_RDSTINTMSK | CSR_TCINTMSK;
      com_csr_ = (com_csr_ & ~kWritable) | (v & kWritable);
      if (v & CSR_RDSTINT) com_csr_ &= ~CSR_RDSTINT;
      if (v & CSR_TCINT) com_csr_ &= ~CSR_TCINT;
      // Start the transfer after updating the interrupt flags.
      if (v & CSR_TSTART) {
        if (retry_event_) hw_.cancel(retry_event_), retry_event_ = 0;
        com_csr_ |= CSR_TSTART;
        run_transfer();
      }
      if (!(com_csr_ & CSR_TSTART)) update_interrupts();
      break;
    }
    case SI_STATUS:
      status_ &= ~(v & ST_ERRORS);
      if (v & ST_WR) {
        // Copy the output registers to the devices.
        for (int i = 0; i < kChannels; ++i) channels_[i].device->send_command(channels_[i].out, (poll_ >> (7 - i)) & 1);
        status_ &= ~(ST_WR | ST_WRST);
      }
      break;
    case SI_EXI_CLOCK_COUNT: exi_clock_count_ = v; break;
  }
}

void SiBus::run_transfer() {
  if (!(com_csr_ & CSR_TSTART)) return;
  const int channel = static_cast<int>((com_csr_ & CSR_CHANNEL) >> 1);
  const int request_length = transfer_length((com_csr_ & CSR_OUTLNGTH) >> 16);
  const int expected_length = transfer_length((com_csr_ & CSR_INLNGTH) >> 8);

  const bool log = log_remaining_ > 0;
  if (log) {
    --log_remaining_;
    std::fprintf(stderr, "[si] ch%d out %d:", channel, request_length);
    for (int i = 0; i < request_length; ++i) std::fprintf(stderr, " %02X", buffer_[i]);
    std::fprintf(stderr, "\n");
  }

  const int response_length = channels_[channel].device->run_buffer(buffer_.data(), request_length);

  if (log) {
    std::fprintf(stderr, "[si] ch%d in %d (expected %d):", channel, response_length, expected_length);
    for (int i = 0; i < response_length && i < expected_length; ++i) std::fprintf(stderr, " %02X", buffer_[i]);
    std::fprintf(stderr, "\n");
  }

  if (response_length == 0) {
    // Not ready yet: try again shortly.
    retry_event_ = hw_.schedule_after(kTimebaseHz / 100000, [this] {
      retry_event_ = 0;
      run_transfer();
    });
    return;
  }
  com_csr_ &= ~(CSR_TSTART | CSR_COMERR);
  if (response_length < 0) {
    com_csr_ |= CSR_COMERR;
    status_ |= st_norep(channel);
  }
  com_csr_ |= CSR_TCINT;
  update_interrupts();
}

void SiBus::update_interrupts() {
  if (status_ & ST_RDST) com_csr_ |= CSR_RDSTINT;
  else com_csr_ &= ~CSR_RDSTINT;
  const bool asserted = ((com_csr_ & CSR_RDSTINT) && (com_csr_ & CSR_RDSTINTMSK)) ||
                        ((com_csr_ & CSR_TCINT) && (com_csr_ & CSR_TCINTMSK));
  hw_.set_interrupt(INT_SI, asserted);
}

}  // namespace hw
