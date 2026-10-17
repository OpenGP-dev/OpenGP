// SPDX-License-Identifier: GPL-3.0-or-later
//
// Field meanings follow Dolphin's BPMemory.h (Copyright Dolphin Emulator
// Project, GPL-2.0-or-later).

#include "gx/describe.h"

#include <cstdio>

namespace gx {

namespace {

std::string format(const char* fmt, auto... args) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return buf;
}

constexpr const char* kCompare[8] = {"never", "less", "equal", "lequal", "greater", "nequal", "gequal", "always"};

constexpr const char* kColorInput[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa",
                                         "c2.rgb",   "c2.aaa",   "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa",
                                         "1",        "0.5",      "konst",   "0"};
constexpr const char* kAlphaInput[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "konst.a", "0"};
constexpr const char* kDest[4] = {"prev", "c0", "c1", "c2"};
constexpr const char* kKonst[32] = {"1",     "7/8",   "3/4",   "5/8",   "1/2",   "3/8",   "1/4",   "1/8",
                                    "?8",    "?9",    "?10",   "?11",   "k0",    "k1",    "k2",    "k3",
                                    "k0.r",  "k1.r",  "k2.r",  "k3.r",  "k0.g",  "k1.g",  "k2.g",  "k3.g",
                                    "k0.b",  "k1.b",  "k2.b",  "k3.b",  "k0.a",  "k1.a",  "k2.a",  "k3.a"};
constexpr const char* kChannel[8] = {"col0", "col1", "?2", "?3", "?4", "bump alpha", "bump alpha (normalised)", "zero"};

// d + lerp(a, b, c) (+ bias) * scale, simplified where an input is 0 or 1.
std::string combiner(const char* a, const char* b, const char* c, const char* d, u32 bias, bool sub, u32 scale,
                     bool clamp) {
  const std::string_view sa = a, sb = b, sc = c, sd = d;
  std::string lerp;
  if (sc == "0") lerp = sa;
  else if (sc == "1") lerp = sb;
  else if (sa == "0" && sb == "0") lerp = "0";
  else if (sa == "0") lerp = std::string(b) + "*" + c;
  else if (sb == "0") lerp = std::string(a) + "*(1-" + c + ")";
  else lerp = "mix(" + std::string(a) + ", " + b + ", " + c + ")";
  std::string out;
  if (sd != "0") out = d;
  if (lerp != "0") {
    if (out.empty()) out = sub ? "-" + lerp : lerp;
    else out += (sub ? " - " : " + ") + lerp;
  }
  if (out.empty()) out = "0";
  if (bias == 1) out += " + 0.5";
  if (bias == 2) out += " - 0.5";
  static constexpr const char* kScale[4] = {"", " *2", " *4", " /2"};
  if (scale) out = "(" + out + ")" + kScale[scale];
  if (!clamp) out += " (unclamped)";
  return out;
}

}  // namespace

const char* texture_format_name(u32 format) {
  switch (format) {
    case 0x0: return "I4";
    case 0x1: return "I8";
    case 0x2: return "IA4";
    case 0x3: return "IA8";
    case 0x4: return "RGB565";
    case 0x5: return "RGB5A3";
    case 0x6: return "RGBA8";
    case 0x8: return "C4";
    case 0x9: return "C8";
    case 0xA: return "C14X2";
    case 0xE: return "CMPR";
    default: return "invalid";
  }
}

const char* copy_format_name(CopyFormat format) {
  static constexpr const char* kNames[16] = {"R4", "R8_1", "RA4", "RA8", "RGB565", "RGB5A3", "RGBA8", "A8",
                                             "R8", "G8",   "B8",  "RG8", "GB8",    "?13",    "?14",   "XFB"};
  return kNames[static_cast<u32>(format) & 15];
}

const char* pixel_format_name(u32 format) {
  static constexpr const char* kNames[8] = {"RGB8_Z24", "RGBA6_Z24", "RGB565_Z16", "Z24", "Y8", "U8", "V8", "YUV420"};
  return kNames[format & 7];
}

const char* primitive_name(u32 primitive) {
  static constexpr const char* kNames[8] = {"quads", "quads2", "triangles", "triangle strip", "triangle fan",
                                            "lines", "line strip", "points"};
  return kNames[primitive & 7];
}

std::string describe_pipeline(u32 k) {
  auto field = [k](u32 shift, u32 count) { return (k >> shift) & ((1u << count) - 1); };
  static constexpr const char* kFactors[8] = {"0", "1", "dst/src colour", "1-colour", "src alpha", "1-src alpha",
                                              "dst alpha", "1-dst alpha"};
  std::string s = field(0, 1) ? std::string("blend ") + kFactors[field(1, 3)] + "," + kFactors[field(4, 3)] +
                                    (field(13, 1) ? " subtract" : "")
                              : "no blend";
  s += field(15, 1) ? ", colour" : ", no colour";
  s += field(16, 1) ? " + alpha writes" : " writes";
  s += field(17, 1) ? std::string(", depth ") + kCompare[field(18, 3)] + (field(21, 1) ? " + write" : "") : ", no depth";
  if (field(22, 1)) s += ", early depth";
  return s;
}

std::string describe_sampler(u32 mode0, u32 mode1) {
  static constexpr const char* kWrap[4] = {"clamp", "repeat", "mirror", "?"};
  static constexpr const char* kMin[8] = {"near", "near mip near", "near mip linear", "?", "linear", "linear mip near",
                                          "linear mip linear", "?"};
  const float bias = static_cast<float>(static_cast<s8>(bits(mode0, 9, 8))) / 32.0f;
  return format("wrap %s,%s, mag %s, min %s, lod %.2f-%.2f bias %.2f", kWrap[bits(mode0, 0, 2)],
                kWrap[bits(mode0, 2, 2)], bits(mode0, 4, 1) ? "linear" : "near", kMin[bits(mode0, 5, 3)],
                static_cast<float>(bits(mode1, 0, 8)) / 16.0f, static_cast<float>(bits(mode1, 8, 8)) / 16.0f, bias);
}

std::string describe_tev(const PixelUniforms& u) {
  std::string out;
  const u32 stages = genmode_num_tev_stages(u.misc[0]);
  for (u32 s = 0; s < stages; ++s) {
    const u32 order = u.orders[s / 2][0] >> ((s & 1) ? 12 : 0);
    const u32 ksel = u.orders[s / 2][1];
    const u32 kc = bits(ksel, (s & 1) ? 14 : 4, 5), ka = bits(ksel, (s & 1) ? 19 : 9, 5);
    const u32 c = u.stages[s][0], a = u.stages[s][1];
    std::string inputs;
    if (bits(order, 6, 1)) inputs += format("tex%u[uv%u] ", bits(order, 0, 3), bits(order, 3, 3));
    inputs += kChannel[bits(order, 7, 3)];
    std::string line = format("    s%u: %s | ", s, inputs.c_str());
    auto konst = [&](std::string text, bool alpha) {
      const std::string name = kKonst[alpha ? ka : kc];
      const std::size_t at = text.find(alpha ? "konst.a" : "konst");
      if (at != std::string::npos) text.replace(at, alpha ? 7 : 5, "k(" + name + ")");
      return text;
    };
    // Colour.
    const u32 cbias = bits(c, 16, 2);
    if (cbias == 3) {  // compare mode
      static constexpr const char* kCmp[8] = {"r8 >", "r8 ==", "gr16 >", "gr16 ==", "bgr24 >", "bgr24 ==", "rgb8 >", "rgb8 =="};
      line += format("%s = %s + (%s %s %s ? %s : 0)", kDest[bits(c, 22, 2)], kColorInput[bits(c, 0, 4)],
                     kColorInput[bits(c, 12, 4)], kCmp[(bits(c, 20, 2) << 1) | bits(c, 18, 1)], kColorInput[bits(c, 8, 4)],
                     kColorInput[bits(c, 4, 4)]);
    } else {
      line += std::string(kDest[bits(c, 22, 2)]) + ".rgb = " +
              combiner(kColorInput[bits(c, 12, 4)], kColorInput[bits(c, 8, 4)], kColorInput[bits(c, 4, 4)],
                       kColorInput[bits(c, 0, 4)], cbias, bits(c, 18, 1), bits(c, 20, 2), bits(c, 19, 1));
    }
    line = konst(line, false);
    // Alpha.
    std::string alpha;
    const u32 abias = bits(a, 16, 2);
    if (abias == 3) {
      static constexpr const char* kCmp[8] = {"r8 >", "r8 ==", "gr16 >", "gr16 ==", "bgr24 >", "bgr24 ==", "a8 >", "a8 =="};
      alpha = format("%s.a = %s + (%s %s %s ? %s : 0)", kDest[bits(a, 22, 2)], kAlphaInput[bits(a, 4, 3)],
                     kAlphaInput[bits(a, 13, 3)], kCmp[(bits(a, 20, 2) << 1) | bits(a, 18, 1)], kAlphaInput[bits(a, 10, 3)],
                     kAlphaInput[bits(a, 7, 3)]);
    } else {
      alpha = std::string(kDest[bits(a, 22, 2)]) + ".a = " +
              combiner(kAlphaInput[bits(a, 13, 3)], kAlphaInput[bits(a, 10, 3)], kAlphaInput[bits(a, 7, 3)],
                       kAlphaInput[bits(a, 4, 3)], abias, bits(a, 18, 1), bits(a, 20, 2), bits(a, 19, 1));
    }
    if (bits(a, 0, 2) || bits(a, 2, 2)) alpha += format(" (swap ras %u tex %u)", bits(a, 0, 2), bits(a, 2, 2));
    line += " | " + konst(alpha, true);
    if (u.stages[s][2]) line += format(" | indirect %06X", u.stages[s][2]);
    out += line + "\n";
  }
  const u32 at = u.misc[1];
  static constexpr const char* kLogic[4] = {"and", "or", "xor", "xnor"};
  if (!(bits(at, 16, 3) == 7 && bits(at, 19, 3) == 7))
    out += format("    alpha test: a %s %u %s a %s %u\n", kCompare[bits(at, 16, 3)], bits(at, 0, 8), kLogic[bits(at, 22, 2)],
                  kCompare[bits(at, 19, 3)], bits(at, 8, 8));
  static constexpr const char* kFog[8] = {"off", "?1", "linear", "?3", "exp", "exp2", "backwards exp", "backwards exp2"};
  if (const u32 fog = bits(u.misc[2], 21, 3)) out += format("    fog: %s%s\n", kFog[fog], bits(u.misc[2], 20, 1) ? " (orthographic)" : "");
  if (u.misc2[3] & 0x10) out += format("    logic op %u\n", u.misc2[3] & 15);
  return out;
}

}  // namespace gx
