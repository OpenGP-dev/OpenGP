// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DSP: the ARAM accelerator's sample decoding and the AX microcode's
// voice mixing, on hand-made voices (no game data).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "dsp/accelerator.h"
#include "dsp/ax.h"
#include "hw/dsp.h"
#include "hw/hardware.h"
#include "ppc/memory.h"

using namespace gekko;

namespace {

class TestAccelerator : public dsp::Accelerator {
 public:
  std::vector<u8> memory = std::vector<u8>(256, 0);
  int ends = 0;

 protected:
  void on_sample_read_end() override { ++ends; }
  u8 read_memory(u32 address) override { return memory[address % memory.size()]; }
};

// The same decoding, written out plainly.
s16 adpcm_reference(int nibble, int scale, s32 c1, s32 c2, s16 yn1, s16 yn2) {
  if (nibble >= 8) nibble -= 16;
  const s32 v = (nibble << scale) + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
  return static_cast<s16>(std::clamp(v, -0x8000, 0x7FFF));
}

// A machine with just memory and the DSP.
struct Rig {
  ppc::GuestMemory memory;
  std::unique_ptr<hw::Hardware> hw;
  hw::DspDevice* dsp = nullptr;

  Rig() {
    REQUIRE(memory.init(ppc::MEM1_SIZE_GAMECUBE));
    hw = std::make_unique<hw::Hardware>(memory.base(), memory.mem1_size());
    auto device = std::make_unique<hw::DspDevice>(*hw);
    dsp = device.get();
    hw->map(0x5000, 0x1000, std::move(device));
    dsp->write(0x0A, 0x0800, 2);  // out of halt (keeping DSPInit set: no init microcode)
    CHECK(mail() == 0x8071FEED);  // the boot ROM's greeting
  }
  void w16(u32 addr, u16 v) { ppc::store_be<u16>(hw->ram(addr, 2), v); }
  u16 r16(u32 addr) { return ppc::load_be<u16>(hw->ram(addr, 2)); }
  u32 mail() { return (u32{static_cast<u16>(dsp->read(0x04, 2))} << 16) | static_cast<u16>(dsp->read(0x06, 2)); }
};

// Word offsets into an AX parameter block.
enum : u32 {
  PB_NEXT_HI = 0,
  PB_SRC_TYPE = 4,
  PB_MIXER_CONTROL = 6,
  PB_RUNNING = 7,
  PB_MAIN_LEFT = 9,  // volume, delta
  PB_MAIN_RIGHT = 11,
  PB_UPDATES = 34,  // 5 counts, then the address
  PB_VOL_ENV = 50,
  PB_LOOPING = 55,
  PB_FORMAT = 56,
  PB_LOOP_ADDR = 57,
  PB_END_ADDR = 59,
  PB_CUR_ADDR = 61,
  PB_GAIN = 79,
};

constexpr u32 kPb = 0x00010000;
constexpr u32 kInit = 0x00011000;  // 27 zero words: the buses start silent
constexpr u32 kCmdList = 0x00012000;
constexpr u32 kOut = 0x00013000;
constexpr u32 kSurround = 0x00014000;
constexpr u32 kUpdates = 0x00015000;

// A 16-bit PCM voice at ARAM sample address `start` .. `end`, mixed to main
// left (volume 1.0) and right (0.5), no resampling.
void write_pcm_voice(Rig& rig, u32 start, u32 end, bool looping) {
  auto pb = [&](u32 word, u16 v) { rig.w16(kPb + word * 2, v); };
  auto pb32 = [&](u32 word, u32 v) {
    pb(word, static_cast<u16>(v >> 16));
    pb(word + 1, static_cast<u16>(v));
  };
  pb32(PB_NEXT_HI, 0);
  pb(PB_SRC_TYPE, 2);          // none
  pb(PB_MIXER_CONTROL, 0x3);   // main L and R
  pb(PB_RUNNING, 1);
  pb(PB_MAIN_LEFT, 0x8000);
  pb(PB_MAIN_RIGHT, 0x4000);
  pb(PB_VOL_ENV, 0x7FFF);
  pb(PB_LOOPING, looping ? 1 : 0);
  pb(PB_FORMAT, 0x0A);  // 16-bit PCM
  pb32(PB_LOOP_ADDR, start);
  pb32(PB_END_ADDR, end);
  pb32(PB_CUR_ADDR, start);
  pb(PB_GAIN, 0x800);
  pb32(PB_UPDATES + 5, kUpdates);
}

// Runs one frame: SETUP, the voice, OUTPUT.
void run_frame(Rig& rig, dsp::AxUcode& ax) {
  auto hi = [](u32 v) { return static_cast<u16>(v >> 16); };
  auto lo = [](u32 v) { return static_cast<u16>(v); };
  const u16 list[] = {0x0000, hi(kInit),     lo(kInit),     0x0002,  hi(kPb),  lo(kPb), 0x0003,
                      0x000E, hi(kSurround), lo(kSurround), hi(kOut), lo(kOut), 0x000F};
  for (u32 i = 0; i < std::size(list); ++i) rig.w16(kCmdList + i * 2, list[i]);
  ax.handle_mail(0xBABE0000 | static_cast<u32>(std::size(list)));
  ax.handle_mail(kCmdList);
}

s16 env(s16 sample) { return static_cast<s16>((s32{sample} * 0x7FFF) >> 15); }
s16 out_left(Rig& rig, u32 i) { return static_cast<s16>(rig.r16(kOut + i * 4 + 2)); }
s16 out_right(Rig& rig, u32 i) { return static_cast<s16>(rig.r16(kOut + i * 4)); }

}  // namespace

TEST_CASE("ADPCM: one frame decodes like the reference, then the end of the sample loops") {
  TestAccelerator acc;
  s16 coefs[16] = {};
  coefs[2 * 2] = 1800;  // predictor 2
  coefs[2 * 2 + 1] = -700;
  // Frame at byte 0: header (predictor 2, scale 3), then 14 nibbles.
  const u8 frame[8] = {0x23, 0x17, 0xF8, 0x3C, 0x90, 0x5A, 0xE2, 0x46};
  std::copy(std::begin(frame), std::end(frame), acc.memory.begin());
  acc.memory[8] = 0x23;  // the next frame's header
  acc.set_sample_format(0);  // ADPCM
  acc.set_start_address(2);  // nibble addresses: after the header
  acc.set_end_address(15);
  acc.set_current_address(2);
  acc.set_pred_scale(0x23);
  acc.set_yn1(0);
  acc.set_yn2(0);

  s16 yn1 = 0, yn2 = 0;
  for (int i = 0; i < 14; ++i) {
    const u8 byte = frame[1 + i / 2];
    const int nibble = (i & 1) ? (byte & 0xF) : (byte >> 4);
    const s16 expected = adpcm_reference(nibble, 3, 1800, -700, yn1, yn2);
    CHECK(static_cast<s16>(acc.read_sample(coefs)) == expected);
    yn2 = yn1;
    yn1 = expected;
  }
  CHECK(acc.ends == 1);
  CHECK(acc.current_address() == 2);
  // Until the loop state is reloaded (YN2 written), reads return silence.
  CHECK(acc.read_sample(coefs) == 0);
  acc.set_yn2(0);
  CHECK(acc.read_sample(coefs) != 0);
}

TEST_CASE("AX: a looping PCM voice is mixed to the output, right channel first") {
  Rig rig;
  const u32 start = 0x1000;  // in 16-bit samples
  for (u32 i = 0; i < 16; ++i) ppc::store_be<u16>(rig.dsp->aram() + (start + i) * 2, static_cast<u16>(i * 1000 - 7000));
  write_pcm_voice(rig, start, start + 15, true);

  dsp::AxUcode ax(*rig.dsp, 0x3AD3B7AC);
  run_frame(rig, ax);

  for (u32 i = 0; i < dsp::AxUcode::kFrameSamples; ++i) {
    const s16 s = env(static_cast<s16>((i % 16) * 1000 - 7000));
    INFO("sample ", i);
    REQUIRE(out_left(rig, i) == s);
    REQUIRE(out_right(rig, i) == static_cast<s16>((s32{s} * 0x4000) >> 15));
  }
  // 160 samples is 10 times round the loop: back at the start, still playing.
  CHECK(rig.r16(kPb + PB_RUNNING * 2) == 1);
  CHECK(rig.r16(kPb + (PB_CUR_ADDR + 1) * 2) == start);
  // The microcode reports the frame done.
  CHECK(rig.mail() == hw::DspUcode::DSP_YIELD);
}

TEST_CASE("AX: a one-shot voice stops at its end") {
  Rig rig;
  const u32 start = 0x2000;
  for (u32 i = 0; i < 40; ++i) ppc::store_be<u16>(rig.dsp->aram() + (start + i) * 2, 3000);
  write_pcm_voice(rig, start, start + 39, false);

  dsp::AxUcode ax(*rig.dsp, 0x3AD3B7AC);
  run_frame(rig, ax);
  CHECK(out_left(rig, 39) == env(3000));
  CHECK(out_left(rig, 40) == 0);
  CHECK(out_left(rig, 159) == 0);
  CHECK(rig.r16(kPb + PB_RUNNING * 2) == 0);
}

TEST_CASE("AX: parameter updates apply at their millisecond") {
  Rig rig;
  const u32 start = 0x3000;
  for (u32 i = 0; i < 16; ++i) ppc::store_be<u16>(rig.dsp->aram() + (start + i) * 2, 2000);
  write_pcm_voice(rig, start, start + 15, true);
  // In the third millisecond (samples 64-95), main left's volume becomes 0.
  rig.w16(kPb + (PB_UPDATES + 2) * 2, 1);
  rig.w16(kUpdates, PB_MAIN_LEFT);
  rig.w16(kUpdates + 2, 0);

  dsp::AxUcode ax(*rig.dsp, 0x3AD3B7AC);
  run_frame(rig, ax);
  CHECK(out_left(rig, 63) == env(2000));
  CHECK(out_left(rig, 64) == 0);
  CHECK(out_left(rig, 159) == 0);
  CHECK(out_right(rig, 159) == env(2000) / 2);
}
