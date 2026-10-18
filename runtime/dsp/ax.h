// SPDX-License-Identifier: GPL-3.0-or-later
//
// High-level emulation of the AX DSP microcode, Nintendo's audio mixer, which
// MusyX drives too. Every 5 ms frame the CPU sends a command list; the
// microcode walks the list of voice parameter blocks (PBs), reads each running
// voice's samples from ARAM through the accelerator, resamples them to 32 kHz,
// applies the voice's envelope, filter and volumes, and mixes it into the main
// and two auxiliary (effects) buses. The CPU gets the aux buses back to run its
// effects, and the final stereo mix is written to main RAM for the audio DMA.
//
// Ported from Dolphin's Core/HW/DSPHLE/UCodes/AX.cpp, AXVoice.h and
// AXStructs.h (Copyright 2008 Dolphin Emulator Project, GPL-2.0-or-later),
// GameCube versions only.
#pragma once

#include <array>
#include <memory>

#include "hw/dsp.h"

namespace dsp {

using namespace gekko;

// True if `crc` (Dolphin's HashEctor of the microcode) is a known AX version.
bool is_ax_ucode(u32 crc);

class AxUcode : public hw::DspUcode {
 public:
  AxUcode(hw::DspDevice& dsp, u32 crc);
  ~AxUcode() override;
  void initialize() override;
  void handle_mail(u32 mail) override;
  const char* name() const override { return "AX"; }

  // One frame: 5 ms of 32 kHz samples.
  static constexpr u32 kFrameSamples = 5 * 32;

 private:
  class VoiceAccelerator;
  struct Pb;
  enum class MailState { CmdListSize, CmdListAddress, NextTask };

  bool copy_cmdlist(u32 addr, u16 size);
  void handle_cmdlist();
  u32 mixer_control(u32 mixer_control) const;

  void setup_processing(u32 init_addr);
  void download_and_mix_with_volume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb);
  void process_pb_list(u32 pb_addr);
  void process_voice(Pb& pb, int* const* buffers, u32 mixer);
  void mix_aux(int aux_id, u32 write_addr, u32 read_addr);
  void upload_lrs(u32 dst_addr);
  void set_main_lr(u32 src_addr);
  void run_compressor(u16 threshold, u16 release_frames, u32 table_addr);
  void output_samples(u32 lr_addr, u32 surround_addr);
  void mix_auxb_lr(u32 ul_addr, u32 dl_addr);
  void set_opposite_lr(u32 src_addr);
  void send_aux_and_mix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                        u32 auxb_r_dl);

  // Guest memory helpers; addresses outside RAM are ignored.
  bool read_be16(u32 addr, u16* dst, u32 count);
  bool write_be16(u32 addr, const u16* src, u32 count);
  bool read_be32(u32 addr, int* dst, u32 count);
  bool write_be32(u32 addr, const int* src, u32 count);

  u32 crc_;
  MailState mail_state_ = MailState::CmdListSize;
  u16 cmdlist_[512]{};
  u16 cmdlist_size_ = 0;

  // The 9 mixing buses: main, aux A, aux B; each left, right, surround.
  enum { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, kBuses };
  int buses_[kBuses][kFrameSamples]{};

  u16 compressor_pos_ = 0;
  std::unique_ptr<VoiceAccelerator> accelerator_;
};

}  // namespace dsp
