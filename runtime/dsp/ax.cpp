// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Dolphin's Core/HW/DSPHLE/UCodes/AX.cpp, AXVoice.h and
// AXStructs.h (Copyright 2008 Dolphin Emulator Project, GPL-2.0-or-later).

#include "dsp/ax.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "dsp/accelerator.h"
#include "ppc/memory.h"

namespace dsp {

namespace {

// AX versions whose command lists and PB layout this implementation handles
// (Dolphin's UCodeFactory).
constexpr u32 kCrcOldAx = 0x4E8A8B21;  // no low-pass filter in the PB, older mixer control bits

// The mixer_control field of a PB means different things in different AX
// versions; it is converted to these bits.
enum : u32 {
  MIX_MAIN_L = 0x000001,
  MIX_MAIN_L_RAMP = 0x000002,
  MIX_MAIN_R = 0x000004,
  MIX_MAIN_R_RAMP = 0x000008,
  MIX_MAIN_S = 0x000010,
  MIX_MAIN_S_RAMP = 0x000020,
  MIX_AUXA_L = 0x000040,
  MIX_AUXA_L_RAMP = 0x000080,
  MIX_AUXA_R = 0x000100,
  MIX_AUXA_R_RAMP = 0x000200,
  MIX_AUXA_S = 0x000400,
  MIX_AUXA_S_RAMP = 0x000800,
  MIX_AUXB_L = 0x001000,
  MIX_AUXB_L_RAMP = 0x002000,
  MIX_AUXB_R = 0x004000,
  MIX_AUXB_R_RAMP = 0x008000,
  MIX_AUXB_S = 0x010000,
  MIX_AUXB_S_RAMP = 0x020000,
  MIX_ALL_RAMPS = 0xAAAAAA,
};

// Command list opcodes.
enum : u16 {
  CMD_SETUP = 0x00,
  CMD_DL_AND_VOL_MIX = 0x01,
  CMD_PB_ADDR = 0x02,
  CMD_PROCESS = 0x03,
  CMD_MIX_AUXA = 0x04,
  CMD_MIX_AUXB = 0x05,
  CMD_UPLOAD_LRS = 0x06,
  CMD_SET_LR = 0x07,
  CMD_UNK_08 = 0x08,
  CMD_MIX_AUXB_NOWRITE = 0x09,
  CMD_UNK_0A = 0x0A,
  CMD_UNK_0B = 0x0B,
  CMD_UNK_0C = 0x0C,
  CMD_MORE = 0x0D,
  CMD_OUTPUT = 0x0E,
  CMD_END = 0x0F,
  CMD_MIX_AUXB_LR = 0x10,
  CMD_SET_OPPOSITE_LR = 0x11,
  CMD_COMPRESSOR = 0x12,
  CMD_SEND_AUX_AND_MIX = 0x13,
};

constexpr u32 MAIL_CMDLIST = 0xBABE0000;
constexpr u32 MAIL_CMDLIST_MASK = 0xFFFF0000;

enum : u16 { SRCTYPE_POLYPHASE = 0, SRCTYPE_LINEAR = 1, SRCTYPE_NEAREST = 2 };

// Time from a command list to the DSP's "done" mail: the empty-list cost
// (Dolphin: 2500 CPU cycles), in time base ticks.
constexpr u64 kCmdListTicks = 2500 * hw::kTimebaseHz / 486000000;

s16 clamp16(s64 v) { return static_cast<s16>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }

}  // namespace

bool is_ax_ucode(u32 crc) {
  switch (crc) {
    case 0x3AD3B7AC:  // Mario Kart Arcade GP 2 (MusyX); Naruto 3, Paper Mario: TTYD
    case 0x3DAF59B9:  // Alien Hominid
    case 0x4E8A8B21:  // older AX: Crazy Taxi, Monkey Ball, Smash Bros. Melee, ...
    case 0x07F88145:  // F-Zero GX, Ikaruga, Soul Calibur 2, ...
    case 0xE2136399:  // Billy Hatcher, Mario Party 5, 1080 Avalanche, ...
      return true;
    default: return false;
  }
}

// --- Parameter blocks --------------------------------------------------------------------

struct VolumeData {
  u16 volume;
  u16 volume_delta;
};

// One voice's parameter block, as u16 words (byte-swapped on load). Update
// lists patch it by word offset.
struct AxUcode::Pb {
  u16 next_pb_hi, next_pb_lo;
  u16 this_pb_hi, this_pb_lo;
  u16 src_type;  // sample rate converter: polyphase, linear, none
  u16 coef_select;
  u16 mixer_control;
  u16 running;    // 1 = playing
  u16 is_stream;  // 1 = stream, else one-shot

  // The odd order of the surround volumes lets the microcode mix two at once.
  struct {
    VolumeData main_left, main_right;
    VolumeData auxA_left, auxA_right;
    VolumeData auxB_left, auxB_right;
    VolumeData auxB_surround, main_surround, auxA_surround;
  } mixer;
  struct {
    u16 on, addr_hi, addr_lo, offset_left, offset_right, target_left, target_right;
  } initial_time_delay;
  // Per-millisecond parameter updates (for MIDI timing within a frame).
  struct {
    u16 num_updates[5];
    u16 data_hi, data_lo;  // in main RAM: pairs of (word offset, value)
  } updates;
  // Last sample of each mix, for de-popping voices that stop.
  struct {
    s16 main_left, auxA_left, auxB_left;
    s16 main_right, auxA_right, auxB_right;
    s16 main_surround, auxA_surround, auxB_surround;
  } dpop;
  struct {
    s16 cur_volume;        // volume at the start of the frame
    s16 cur_volume_delta;  // per sample
  } vol_env;
  u16 unknown3[3];
  struct {
    u16 looping;
    u16 sample_format;
    u16 loop_addr_hi, loop_addr_lo;
    u16 end_addr_hi, end_addr_lo;  // inclusive
    u16 cur_addr_hi, cur_addr_lo;
  } audio_addr;
  struct {
    s16 coefs[16];
    u16 gain;
    u16 pred_scale;
    s16 yn1, yn2;
  } adpcm;
  struct {
    u16 ratio_hi, ratio_lo;  // input/output ratio, 16.16
    u16 cur_addr_frac;
    s16 last_samples[4];
  } src;
  struct {
    u16 pred_scale;
    u16 yn1, yn2;
  } adpcm_loop_info;
  struct {
    u16 on;
    s16 yn1;
    u16 a0;
    s16 b0;
  } lpf;  // not in the older AX layout
  u16 loop_counter;
  u16 padding[24];
};

#define HILO(name) ((u32{name##_hi} << 16) | name##_lo)

// The accelerator reading a voice's samples from ARAM. At the end of the
// sample it loops (reloading the ADPCM state from the PB) or stops the voice.
class AxUcode::VoiceAccelerator final : public Accelerator {
 public:
  explicit VoiceAccelerator(hw::DspDevice& dsp) : dsp_(dsp) {}
  Pb* pb = nullptr;

 protected:
  void on_sample_read_end() override {
    if (pb->audio_addr.looping) {
      set_pred_scale(pb->adpcm_loop_info.pred_scale);
      if (pb->is_stream != 1) {
        set_yn1(static_cast<s16>(pb->adpcm_loop_info.yn1));
        set_yn2(static_cast<s16>(pb->adpcm_loop_info.yn2));
      } else {
        // Streams keep their history; rewriting YN2 resumes the reads.
        set_yn1(yn1());
        set_yn2(yn2());
        pb->loop_counter++;
      }
    } else {
      pb->running = 0;
    }
  }
  u8 read_memory(u32 address) override { return dsp_.aram()[address & (dsp_.aram_size() - 1)]; }

 private:
  hw::DspDevice& dsp_;
};

// --- Mail -----------------------------------------------------------------------------------

AxUcode::AxUcode(hw::DspDevice& dsp, u32 crc)
    : DspUcode(dsp, crc), crc_(crc), accelerator_(std::make_unique<VoiceAccelerator>(dsp)) {}

AxUcode::~AxUcode() = default;

void AxUcode::initialize() { dsp_.push_mail(DSP_INIT, true); }

void AxUcode::handle_mail(u32 mail) {
  if (upload_in_progress_) {
    prepare_boot_ucode(mail);
    return;
  }
  switch (mail_state_) {
    case MailState::CmdListSize:
      if ((mail & MAIL_CMDLIST_MASK) == MAIL_CMDLIST) {
        cmdlist_size_ = static_cast<u16>(mail);
        mail_state_ = MailState::CmdListAddress;
      } else {
        std::fprintf(stderr, "[dsp] AX: unexpected mail %08X (expected a command list)\n", mail);
      }
      break;
    case MailState::CmdListAddress:
      if (copy_cmdlist(mail, cmdlist_size_)) handle_cmdlist();
      cmdlist_size_ = 0;
      dsp_.push_mail(DSP_YIELD, true, kCmdListTicks);
      mail_state_ = MailState::NextTask;
      break;
    case MailState::NextTask:
      // The microcode does not check the CDD1 prefix.
      switch (TASK_MAIL_TO_DSP | (mail & 0xFFFF)) {
        case MAIL_RESUME:
          dsp_.push_mail(DSP_RESUME, true);
          mail_state_ = MailState::CmdListSize;
          break;
        case MAIL_NEW_UCODE:
          upload_in_progress_ = true;
          mail_state_ = MailState::CmdListSize;
          break;
        case MAIL_RESET: dsp_.reset_ucode(); return;  // this object has been replaced
        case MAIL_CONTINUE: mail_state_ = MailState::CmdListSize; break;
        default: std::fprintf(stderr, "[dsp] AX: unknown task mail %08X\n", mail); break;
      }
      break;
  }
}

bool AxUcode::copy_cmdlist(u32 addr, u16 size) {
  if (size == 0 || size >= std::size(cmdlist_)) {
    std::fprintf(stderr, "[dsp] AX: command list at %08X has a bad size (%u words)\n", addr, size);
    return false;
  }
  if (!read_be16(addr, cmdlist_, size)) {
    std::fprintf(stderr, "[dsp] AX: command list at %08X is outside memory\n", addr);
    return false;
  }
  return true;
}

// --- Guest memory ------------------------------------------------------------------------

bool AxUcode::read_be16(u32 addr, u16* dst, u32 count) {
  const u8* p = dsp_.ram(addr, count * 2);
  if (!p) return false;
  for (u32 i = 0; i < count; ++i) dst[i] = ppc::load_be<u16>(p + i * 2);
  return true;
}

bool AxUcode::write_be16(u32 addr, const u16* src, u32 count) {
  u8* p = dsp_.ram(addr, count * 2);
  if (!p) return false;
  for (u32 i = 0; i < count; ++i) ppc::store_be<u16>(p + i * 2, src[i]);
  return true;
}

bool AxUcode::read_be32(u32 addr, int* dst, u32 count) {
  const u8* p = dsp_.ram(addr, count * 4);
  if (!p) return false;
  for (u32 i = 0; i < count; ++i) dst[i] = static_cast<int>(ppc::load_be<u32>(p + i * 4));
  return true;
}

bool AxUcode::write_be32(u32 addr, const int* src, u32 count) {
  u8* p = dsp_.ram(addr, count * 4);
  if (!p) return false;
  for (u32 i = 0; i < count; ++i) ppc::store_be<u32>(p + i * 4, static_cast<u32>(src[i]));
  return true;
}

// --- Command lists ----------------------------------------------------------------------

void AxUcode::handle_cmdlist() {
  u32 pb_addr = 0;
  u32 idx = 0;
  auto next = [&] { return idx < std::size(cmdlist_) ? cmdlist_[idx++] : u16{CMD_END}; };
  auto next32 = [&] {
    const u32 hi = next();
    return (hi << 16) | next();
  };
  // A list that keeps chaining to itself (CMD_MORE) ends after this many
  // commands, rather than holding the game's thread for ever.
  for (int commands = 0; commands < 65536; ++commands) {
    const u16 cmd = next();
    switch (cmd) {
      case CMD_SETUP: setup_processing(next32()); break;
      case CMD_DL_AND_VOL_MIX: {
        const u32 addr = next32();
        const u16 vol_main = next(), vol_auxa = next(), vol_auxb = next();
        download_and_mix_with_volume(addr, vol_main, vol_auxa, vol_auxb);
        break;
      }
      case CMD_PB_ADDR: pb_addr = next32(); break;
      case CMD_PROCESS: process_pb_list(pb_addr); break;
      case CMD_MIX_AUXA:
      case CMD_MIX_AUXB: {
        const u32 write_addr = next32();
        const u32 read_addr = next32();
        mix_aux(cmd - CMD_MIX_AUXA, write_addr, read_addr);
        break;
      }
      case CMD_UPLOAD_LRS: upload_lrs(next32()); break;
      case CMD_SET_LR: set_main_lr(next32()); break;
      case CMD_UNK_08: idx += 10; break;
      case CMD_MIX_AUXB_NOWRITE: mix_aux(1, 0, next32()); break;
      case CMD_UNK_0A:
      case CMD_UNK_0B:
      case CMD_UNK_0C: break;  // no-ops
      case CMD_MORE: {
        const u32 addr = next32();
        const u16 size = next();
        // A list it cannot copy ends here (replaying the old one would loop
        // for ever on the game's thread).
        if (!copy_cmdlist(addr, size)) return;
        idx = 0;
        break;
      }
      case CMD_OUTPUT: {
        const u32 surround_addr = next32();
        const u32 lr_addr = next32();
        output_samples(lr_addr, surround_addr);
        break;
      }
      case CMD_END: return;
      case CMD_MIX_AUXB_LR: {
        const u32 ul_addr = next32();
        const u32 dl_addr = next32();
        mix_auxb_lr(ul_addr, dl_addr);
        break;
      }
      case CMD_SET_OPPOSITE_LR: set_opposite_lr(next32()); break;
      case CMD_COMPRESSOR: {
        const u16 threshold = next();
        const u16 frames = next();
        run_compressor(threshold, frames, next32());
        break;
      }
      case CMD_SEND_AUX_AND_MIX: {
        u32 a[6];
        for (u32& v : a) v = next32();
        send_aux_and_mix(a[0], a[1], a[2], a[3], a[4], a[5]);
        break;
      }
      default: std::fprintf(stderr, "[dsp] AX: unknown command %04X\n", cmd); return;
    }
  }
}

u32 AxUcode::mixer_control(u32 mc) const {
  u32 ret = 0;
  if (crc_ == kCrcOldAx) {
    if (mc & 0x0010) {
      // Dolby Pro Logic II
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if ((mc & 0x0006) == 0) ret |= MIX_AUXB_L | MIX_AUXB_R;
      if ((mc & 0x0007) == 1) ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
    } else {
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if (mc & 0x0001) ret |= MIX_AUXA_L | MIX_AUXA_R;
      if (mc & 0x0002) ret |= MIX_AUXB_L | MIX_AUXB_R;
      if (mc & 0x0004) {
        ret |= MIX_MAIN_S;
        if (ret & MIX_AUXA_L) ret |= MIX_AUXA_S;
        if (ret & MIX_AUXB_L) ret |= MIX_AUXB_S;
      }
    }
    if (mc & 0x0008) ret |= MIX_ALL_RAMPS;
    return ret;
  }
  if (mc & 0x0001) ret |= MIX_MAIN_L;
  if (mc & 0x0002) ret |= MIX_MAIN_R;
  if (mc & 0x0004) ret |= MIX_MAIN_S;
  if (mc & 0x0008) ret |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
  if (mc & 0x0010) ret |= MIX_AUXA_L;
  if (mc & 0x0020) ret |= MIX_AUXA_R;
  if (mc & 0x0040) ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
  if (mc & 0x0080) ret |= MIX_AUXA_S;
  if (mc & 0x0100) ret |= MIX_AUXA_S_RAMP;
  if (mc & 0x0200) ret |= MIX_AUXB_L;
  if (mc & 0x0400) ret |= MIX_AUXB_R;
  if (mc & 0x0800) ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
  if (mc & 0x1000) ret |= MIX_AUXB_S;
  if (mc & 0x2000) ret |= MIX_AUXB_S_RAMP;
  // 0x4000 (Dolby Pro Logic II surround for AUXB) needs initial time delay support.
  return ret;
}

// Initial values for the 9 buses: a start value and a per-sample delta each.
void AxUcode::setup_processing(u32 init_addr) {
  u16 init[3 * kBuses];
  if (!read_be16(init_addr, init, std::size(init))) return;
  for (u32 b = 0; b < kBuses; ++b) {
    const s32 value = static_cast<s32>((u32{init[3 * b]} << 16) | init[3 * b + 1]);
    const s16 delta = static_cast<s16>(init[3 * b + 2]);
    for (u32 i = 0; i < kFrameSamples; ++i) buses_[b][i] = value ? value + static_cast<int>(i) * delta : 0;
  }
}

// Mixes one downloaded L/R/S frame into the main, aux A and aux B buses, each
// with its own volume.
void AxUcode::download_and_mix_with_volume(u32 addr, u16 vol_main, u16 vol_auxa, u16 vol_auxb) {
  int in[3 * kFrameSamples];
  if (!read_be32(addr, in, std::size(in))) return;
  const u16 volumes[3] = {vol_main, vol_auxa, vol_auxb};
  for (u32 b = 0; b < kBuses; ++b) {
    const s64 volume = volumes[b / 3];
    const int* src = in + (b % 3) * kFrameSamples;
    for (u32 i = 0; i < kFrameSamples; ++i) buses_[b][i] += static_cast<s32>((s64{src[i]} * volume) >> 15);
  }
}

// --- Voices -------------------------------------------------------------------------------

void AxUcode::process_pb_list(u32 pb_addr) {
  const bool has_lpf = crc_ != kCrcOldAx;
  constexpr u32 kWords = sizeof(Pb) / 2;
  constexpr u32 kLpfWord = offsetof(Pb, lpf) / 2;
  constexpr u32 kAfterLpfWord = offsetof(Pb, loop_counter) / 2;
  // The older layout has no low-pass filter: the words after it move up.
  const u32 stored_words = has_lpf ? kWords : kWords - (kAfterLpfWord - kLpfWord);

  // Guard against lists that loop back on themselves.
  for (int guard = 0; pb_addr && guard < 1024; ++guard) {
    Pb pb{};
    u16* words = reinterpret_cast<u16*>(&pb);
    u16 raw[kWords];
    if (!read_be16(pb_addr, raw, stored_words)) break;
    if (has_lpf) {
      std::memcpy(words, raw, sizeof(raw));
    } else {
      std::memcpy(words, raw, kLpfWord * 2);
      std::memcpy(words + kAfterLpfWord, raw + kLpfWord, (kWords - kAfterLpfWord) * 2);
    }

    // Parameter updates for each millisecond of the frame.
    struct Update {
      u16 offset, value;
    } updates[32]{};
    // Not applied if the list is outside memory (zeroed updates would write
    // word 0, the link to the next voice).
    const bool have_updates = read_be16(HILO(pb.updates.data), reinterpret_cast<u16*>(updates), 64);

    int* buffers[kBuses];
    for (u32 b = 0; b < kBuses; ++b) buffers[b] = buses_[b];
    for (int ms = 0; ms < 5; ++ms) {
      // The counts are read live: an update may change them.
      u32 first = 0;
      for (int i = 0; i < ms; ++i) first += pb.updates.num_updates[i];
      const u32 count = pb.updates.num_updates[ms];
      if (first < std::size(updates) && count <= std::size(updates) - first) {
        for (u32 i = first; i < first + count; ++i)
          if (have_updates && updates[i].offset < kWords) words[updates[i].offset] = updates[i].value;
      }

      process_voice(pb, buffers, mixer_control(pb.mixer_control));
      for (int*& p : buffers) p += 32;
    }

    if (has_lpf) {
      std::memcpy(raw, words, sizeof(raw));
    } else {
      std::memcpy(raw, words, kLpfWord * 2);
      std::memcpy(raw + kLpfWord, words + kAfterLpfWord, (kWords - kAfterLpfWord) * 2);
    }
    write_be16(pb_addr, raw, stored_words);
    pb_addr = HILO(pb.next_pb);
  }
}

namespace {

// Resamples `count` output samples from the accelerator's input at `ratio`
// (16.16 input samples per output sample), interpolating linearly between the
// last input samples. Returns the new fractional position.
template <typename Input>
u32 resample(Input input, s16* output, u32 count, s16* last_samples, u32 pos, u32 ratio, u16 src_type) {
  if (src_type == SRCTYPE_NEAREST) {
    for (u32 i = 0; i < count; ++i) output[i] = static_cast<s16>(input());
    std::memcpy(last_samples, output + count - 4, 4 * sizeof(s16));
    return pos;
  }
  // Polyphase needs the DSP ROM's coefficient table; like Dolphin without
  // it, fall back to linear interpolation.
  s16 temp[4];
  u32 idx = 0;
  for (int i = 0; i < 4; ++i) temp[idx++ & 3] = last_samples[i];
  for (u32 i = 0; i < count; ++i) {
    pos += ratio;
    while (pos >= 0x10000) {
      temp[idx++ & 3] = static_cast<s16>(input());
      pos -= 0x10000;
    }
    const u16 frac = pos & 0xFFFF;
    const u16 inv_frac = static_cast<u16>(-frac);
    if (frac) {
      const s32 s0 = temp[idx++ & 3];
      const s32 s1 = temp[idx++ & 3];
      output[i] = static_cast<s16>((s0 * inv_frac + s1 * frac) >> 16);
      idx += 2;
    } else {
      output[i] = temp[idx++ & 3];
      idx += 3;
    }
  }
  for (int i = 3; i >= 0; --i) last_samples[i] = temp[--idx & 3];
  return pos;
}

void mix_add(int* out, const s16* input, u32 count, VolumeData& vd, s16& dpop, bool ramp) {
  const u16 delta = ramp ? vd.volume_delta : 0;
  for (u32 i = 0; i < count; ++i) {
    const s16 sample = clamp16((s64{input[i]} * vd.volume) >> 15);
    out[i] += sample;
    vd.volume = static_cast<u16>(vd.volume + delta);
    dpop = sample;
  }
}

}  // namespace

// Renders 1 ms (32 samples) of a voice into the buses.
void AxUcode::process_voice(Pb& pb, int* const* buffers, u32 mixer) {
  if (pb.running != 1) return;
  constexpr u32 kCount = 32;

  // Read and resample the input.
  VoiceAccelerator& acc = *accelerator_;
  acc.pb = &pb;
  acc.set_start_address(HILO(pb.audio_addr.loop_addr));
  acc.set_end_address(HILO(pb.audio_addr.end_addr));
  acc.set_current_address(HILO(pb.audio_addr.cur_addr));
  acc.set_sample_format(pb.audio_addr.sample_format);
  acc.set_yn1(pb.adpcm.yn1);
  acc.set_yn2(pb.adpcm.yn2);
  acc.set_gain(static_cast<s16>(pb.adpcm.gain));
  acc.set_pred_scale(pb.adpcm.pred_scale);
  s16 samples[kCount];
  const s16* coefs = pb.adpcm.coefs;
  pb.src.cur_addr_frac = static_cast<u16>(resample([&] { return acc.read_sample(coefs); }, samples, kCount,
                                                   pb.src.last_samples, pb.src.cur_addr_frac, HILO(pb.src.ratio),
                                                   pb.src_type));
  pb.audio_addr.cur_addr_hi = static_cast<u16>(acc.current_address() >> 16);
  pb.audio_addr.cur_addr_lo = static_cast<u16>(acc.current_address());
  pb.adpcm.yn1 = acc.yn1();
  pb.adpcm.yn2 = acc.yn2();
  pb.adpcm.pred_scale = acc.pred_scale();

  // Volume envelope (signed on the GameCube).
  for (s16& s : samples) {
    s = clamp16((s32{s} * pb.vol_env.cur_volume) >> 15);
    pb.vol_env.cur_volume = static_cast<s16>(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
  }

  // One-pole low-pass filter.
  if (pb.lpf.on) {
    for (s16& s : samples) {
      pb.lpf.yn1 = clamp16((s32{pb.lpf.a0} * s + s32{pb.lpf.b0} * pb.lpf.yn1) >> 15);
      s = pb.lpf.yn1;
    }
  }

  auto mix = [&](u32 on, u32 ramp, int bus, VolumeData& vd, s16& dpop) {
    if (mixer & on) mix_add(buffers[bus], samples, kCount, vd, dpop, (mixer & ramp) != 0);
  };
  mix(MIX_MAIN_L, MIX_MAIN_L_RAMP, MAIN_L, pb.mixer.main_left, pb.dpop.main_left);
  mix(MIX_MAIN_R, MIX_MAIN_R_RAMP, MAIN_R, pb.mixer.main_right, pb.dpop.main_right);
  mix(MIX_MAIN_S, MIX_MAIN_S_RAMP, MAIN_S, pb.mixer.main_surround, pb.dpop.main_surround);
  mix(MIX_AUXA_L, MIX_AUXA_L_RAMP, AUXA_L, pb.mixer.auxA_left, pb.dpop.auxA_left);
  mix(MIX_AUXA_R, MIX_AUXA_R_RAMP, AUXA_R, pb.mixer.auxA_right, pb.dpop.auxA_right);
  mix(MIX_AUXA_S, MIX_AUXA_S_RAMP, AUXA_S, pb.mixer.auxA_surround, pb.dpop.auxA_surround);
  mix(MIX_AUXB_L, MIX_AUXB_L_RAMP, AUXB_L, pb.mixer.auxB_left, pb.dpop.auxB_left);
  mix(MIX_AUXB_R, MIX_AUXB_R_RAMP, AUXB_R, pb.mixer.auxB_right, pb.dpop.auxB_right);
  mix(MIX_AUXB_S, MIX_AUXB_S_RAMP, AUXB_S, pb.mixer.auxB_surround, pb.dpop.auxB_surround);
  // Initial time delay (a phase shift for 3D sound) is not emulated.
}

// --- Buses --------------------------------------------------------------------------------

// Sends an aux bus to the CPU (for its effects), then mixes the CPU's
// returned samples into the main bus.
void AxUcode::mix_aux(int aux_id, u32 write_addr, u32 read_addr) {
  const int first = aux_id == 0 ? AUXA_L : AUXB_L;
  if (write_addr) {
    for (int b = 0; b < 3; ++b) write_be32(write_addr + b * kFrameSamples * 4, buses_[first + b], kFrameSamples);
  }
  int in[3 * kFrameSamples];
  if (!read_be32(read_addr, in, std::size(in))) return;
  for (int b = 0; b < 3; ++b)
    for (u32 i = 0; i < kFrameSamples; ++i) buses_[MAIN_L + b][i] += in[b * kFrameSamples + i];
}

void AxUcode::upload_lrs(u32 dst_addr) {
  for (int b = 0; b < 3; ++b) write_be32(dst_addr + b * kFrameSamples * 4, buses_[MAIN_L + b], kFrameSamples);
}

void AxUcode::set_main_lr(u32 src_addr) {
  int in[kFrameSamples];
  if (!read_be32(src_addr, in, kFrameSamples)) return;
  for (u32 i = 0; i < kFrameSamples; ++i) {
    buses_[MAIN_L][i] = in[i];
    buses_[MAIN_R][i] = in[i];
    buses_[MAIN_S][i] = 0;
  }
}

void AxUcode::run_compressor(u16 threshold, u16 release_frames, u32 table_addr) {
  bool triggered = false;
  for (u32 i = 0; i < kFrameSamples && !triggered; ++i)
    triggered = std::abs(buses_[MAIN_L][i]) > int{threshold} || std::abs(buses_[MAIN_R][i]) > int{threshold};

  constexpr u32 kFrameBytes = kFrameSamples * 2;
  u32 table_offset = 0;
  if (triggered) {
    // One attack frame, chosen by where the previous release got to.
    table_offset = compressor_pos_ * kFrameBytes;
    compressor_pos_ = release_frames;
  } else if (compressor_pos_) {
    // Release ramps follow the 11 attack ramps.
    --compressor_pos_;
    table_offset = (11 + compressor_pos_) * kFrameBytes;
  } else {
    return;
  }
  u16 ramp[kFrameSamples];
  if (!read_be16(table_addr + table_offset, ramp, kFrameSamples)) return;
  for (u32 i = 0; i < kFrameSamples; ++i) {
    buses_[MAIN_L][i] = static_cast<int>((s64{buses_[MAIN_L][i]} * ramp[i]) >> 15);
    buses_[MAIN_R][i] = static_cast<int>((s64{buses_[MAIN_R][i]} * ramp[i]) >> 15);
  }
}

// The final mix: surround to `surround_addr`, and stereo 16-bit samples,
// right then left, to `lr_addr` for the audio DMA.
void AxUcode::output_samples(u32 lr_addr, u32 surround_addr) {
  write_be32(surround_addr, buses_[MAIN_S], kFrameSamples);
  u16 out[2 * kFrameSamples];
  for (u32 i = 0; i < kFrameSamples; ++i) {
    out[2 * i + 0] = static_cast<u16>(clamp16(buses_[MAIN_R][i]));
    out[2 * i + 1] = static_cast<u16>(clamp16(buses_[MAIN_L][i]));
  }
  write_be16(lr_addr, out, std::size(out));
}

void AxUcode::mix_auxb_lr(u32 ul_addr, u32 dl_addr) {
  write_be32(ul_addr, buses_[AUXB_L], kFrameSamples);
  write_be32(ul_addr + kFrameSamples * 4, buses_[AUXB_R], kFrameSamples);
  int in[2 * kFrameSamples];
  if (!read_be32(dl_addr, in, std::size(in))) return;
  for (u32 i = 0; i < kFrameSamples; ++i) {
    buses_[AUXB_L][i] = in[i];
    buses_[MAIN_L][i] += in[i];
    buses_[AUXB_R][i] = in[kFrameSamples + i];
    buses_[MAIN_R][i] += in[kFrameSamples + i];
  }
}

void AxUcode::set_opposite_lr(u32 src_addr) {
  int in[kFrameSamples];
  if (!read_be32(src_addr, in, kFrameSamples)) return;
  for (u32 i = 0; i < kFrameSamples; ++i) {
    buses_[MAIN_L][i] = -in[i];
    buses_[MAIN_R][i] = in[i];
    buses_[MAIN_S][i] = 0;
  }
}

// Sends aux A (all three channels) and aux B surround to the CPU, then mixes
// the CPU's samples into main L/R and aux B L/R.
void AxUcode::send_aux_and_mix(u32 auxa_lrs_up, u32 auxb_s_up, u32 main_l_dl, u32 main_r_dl, u32 auxb_l_dl,
                               u32 auxb_r_dl) {
  for (int b = 0; b < 3; ++b) write_be32(auxa_lrs_up + b * kFrameSamples * 4, buses_[AUXA_L + b], kFrameSamples);
  write_be32(auxb_s_up, buses_[AUXB_S], kFrameSamples);
  const int dl_buses[4] = {MAIN_L, MAIN_R, AUXB_L, AUXB_R};
  const u32 dl_addrs[4] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
  for (int i = 0; i < 4; ++i) {
    int in[kFrameSamples];
    if (!read_be32(dl_addrs[i], in, kFrameSamples)) continue;
    for (u32 j = 0; j < kFrameSamples; ++j) buses_[dl_buses[i]][j] += in[j];
  }
}

}  // namespace dsp
