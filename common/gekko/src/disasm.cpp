// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <string>

#include "gekko/isa.h"

namespace gekko {

namespace {

std::string hex(s64 v) {
  char buf[32];
  if (v < 0)
    std::snprintf(buf, sizeof(buf), "-0x%llx", static_cast<unsigned long long>(-v));
  else
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string addr(u32 a) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", a);
  return buf;
}

const char* spr_name(u32 spr) {
  switch (spr) {
    case 1: return "XER";
    case 8: return "LR";
    case 9: return "CTR";
    case 18: return "DSISR";
    case 19: return "DAR";
    case 22: return "DEC";
    case 25: return "SDR1";
    case 26: return "SRR0";
    case 27: return "SRR1";
    case 268: return "TBL";
    case 269: return "TBU";
    case 272: return "SPRG0";
    case 273: return "SPRG1";
    case 274: return "SPRG2";
    case 275: return "SPRG3";
    case 282: return "EAR";
    case 284: return "TBL_W";
    case 285: return "TBU_W";
    case 287: return "PVR";
    case 912: return "GQR0";
    case 913: return "GQR1";
    case 914: return "GQR2";
    case 915: return "GQR3";
    case 916: return "GQR4";
    case 917: return "GQR5";
    case 918: return "GQR6";
    case 919: return "GQR7";
    case 920: return "HID2";
    case 921: return "WPAR";
    case 922: return "DMA_U";
    case 923: return "DMA_L";
    case 1008: return "HID0";
    case 1009: return "HID1";
    case 1017: return "L2CR";
    default: return nullptr;
  }
}

std::string operand(const FieldDesc& f, s64 v, u32 pc, bool absolute) {
  switch (f.kind) {
    case FieldKind::Gpr: return "r" + std::to_string(v);
    case FieldKind::Fpr: return "f" + std::to_string(v);
    case FieldKind::Crf: return "cr" + std::to_string(v);
    case FieldKind::Crb: return std::to_string(v);
    case FieldKind::Dec: return std::to_string(v);
    case FieldKind::Hex:
    case FieldKind::UImm:
    case FieldKind::SImm:
    case FieldKind::Disp: return hex(v);
    case FieldKind::Spr: {
      const char* n = spr_name(static_cast<u32>(v));
      return n ? n : std::to_string(v);
    }
    case FieldKind::BranchTarget:
      return addr(absolute ? static_cast<u32>(v) : pc + static_cast<u32>(v));
    case FieldKind::Flag: return {};
  }
  return {};
}

const char* cond_name(u32 bit) {
  static const char* const kNames[] = {"lt", "gt", "eq", "so"};
  return kNames[bit & 3];
}

// Simplified mnemonics for conditional branches: "beq cr1, target", "bdnz target", ...
bool simplify_bc(u32 raw, u32 pc, const OpInfo& info, std::string& out) {
  const u32 bo = fields::bo::get(raw);
  const u32 bi = fields::bi::get(raw);
  const u32 lk = fields::lk::get(raw);
  std::string suffix;
  std::string target;
  if (info.op == Op::bc) {
    const u32 aa = fields::aa::get(raw);
    const s32 bd = fields::bd::get(raw);
    target = addr(aa ? static_cast<u32>(bd) : pc + static_cast<u32>(bd));
    if (lk) suffix += "l";
    if (aa) suffix += "a";
  } else {
    suffix = info.op == Op::bclr ? "lr" : "ctr";
    if (lk) suffix += "l";
  }
  // Ignore the branch-prediction hint bit (bo & 1).
  const u32 bo_nohint = bo & ~1u;
  std::string cr = (bi >> 2) ? "cr" + std::to_string(bi >> 2) : "";
  auto join = [&](const std::string& mn, const std::string& a, const std::string& b) {
    out = mn;
    std::string ops = a;
    if (!b.empty()) ops += (ops.empty() ? "" : ", ") + b;
    if (!ops.empty()) out += " " + ops;
  };
  switch (bo_nohint) {
    case 20: join("b" + suffix, "", target); return true;
    case 12: join(std::string("b") + cond_name(bi) + suffix, cr, target); return true;
    case 4: {
      static const char* const kNeg[] = {"ge", "le", "ne", "ns"};
      join(std::string("b") + kNeg[bi & 3] + suffix, cr, target);
      return true;
    }
    case 16: join("bdnz" + suffix, "", target); return true;
    case 18: join("bdz" + suffix, "", target); return true;
    default: return false;
  }
}

bool simplify(u32 raw, u32 pc, const OpInfo& info, std::string& out) {
  using namespace fields;
  const u32 d = rD::get(raw), a = rA::get(raw), b = rB::get(raw);
  switch (info.op) {
    case Op::addi:
      if (a == 0) { out = "li r" + std::to_string(d) + ", " + hex(simm::get(raw)); return true; }
      return false;
    case Op::addis:
      if (a == 0) { out = "lis r" + std::to_string(d) + ", " + hex(uimm::get(raw)); return true; }
      return false;
    case Op::or_:
      if (d == b && !rc::get(raw)) {
        out = "mr r" + std::to_string(a) + ", r" + std::to_string(d);
        return true;
      }
      return false;
    case Op::ori:
      if (raw == 0x60000000) { out = "nop"; return true; }
      return false;
    case Op::nor:
      if (d == b) {
        out = std::string(rc::get(raw) ? "not. r" : "not r") + std::to_string(a) + ", r" +
              std::to_string(d);
        return true;
      }
      return false;
    case Op::mfspr:
    case Op::mtspr: {
      const u32 s = spr::get(raw);
      const char* n = s == 8 ? "lr" : s == 9 ? "ctr" : s == 1 ? "xer" : nullptr;
      if (!n) return false;
      out = std::string(info.op == Op::mfspr ? "mf" : "mt") + n + " r" + std::to_string(d);
      return true;
    }
    case Op::cmpi:
    case Op::cmpli:
    case Op::cmp:
    case Op::cmpl: {
      const u32 crf = crfD::get(raw);
      std::string mn = info.op == Op::cmpi ? "cmpwi" : info.op == Op::cmpli ? "cmplwi"
                     : info.op == Op::cmp  ? "cmpw"  : "cmplw";
      out = mn + " ";
      if (crf) out += "cr" + std::to_string(crf) + ", ";
      out += "r" + std::to_string(a) + ", ";
      if (info.op == Op::cmpi) out += hex(simm::get(raw));
      else if (info.op == Op::cmpli) out += hex(uimm::get(raw));
      else out += "r" + std::to_string(b);
      return true;
    }
    case Op::rlwinm: {
      const u32 sh = fields::sh::get(raw), mb = fields::mb::get(raw), me = fields::me::get(raw);
      const std::string dot = rc::get(raw) ? "." : "";
      const std::string regs = " r" + std::to_string(a) + ", r" + std::to_string(d) + ", ";
      if (mb == 0 && me == 31 - sh) { out = "slwi" + dot + regs + std::to_string(sh); return true; }
      if (me == 31 && sh != 0 && mb == 32 - sh) {
        out = "srwi" + dot + regs + std::to_string(mb);
        return true;
      }
      if (sh == 0 && me == 31) { out = "clrlwi" + dot + regs + std::to_string(mb); return true; }
      return false;
    }
    case Op::bc:
    case Op::bclr:
    case Op::bcctr:
      return simplify_bc(raw, pc, info, out);
    default:
      return false;
  }
}

}  // namespace

std::string disasm(u32 raw, u32 pc) {
  const Op op = decode(raw);
  if (op == Op::Invalid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), ".long 0x%08X", raw);
    return buf;
  }
  const OpInfo& info = op_info(op);

  std::string out;
  if (simplify(raw, pc, info, out)) return out;

  const auto flds = sig_fields(info.sig);

  // Mnemonic with suffixes from flag fields.
  std::string mn = info.mnemonic;
  bool absolute = false;
  for (const FieldDesc& f : flds) {
    if (f.kind != FieldKind::Flag || field_get(f, raw) == 0) continue;
    const std::string n = f.name;
    if (n == "oe") mn += "o";
  }
  for (const FieldDesc& f : flds) {
    if (f.kind != FieldKind::Flag || field_get(f, raw) == 0) continue;
    const std::string n = f.name;
    if (n == "rc") mn += ".";
    else if (n == "lk") mn += "l";
    else if (n == "aa") { mn += "a"; absolute = true; }
  }

  // Memory forms: rD, d(rA)[, W, I]
  const bool dform = info.sig == Sig::D_LOAD || info.sig == Sig::D_STORE ||
                     info.sig == Sig::D_FLOAD || info.sig == Sig::D_FSTORE ||
                     info.sig == Sig::PSQ_D;
  std::string ops;
  auto add = [&](const std::string& s) {
    if (s.empty()) return;
    if (!ops.empty()) ops += ", ";
    ops += s;
  };
  if (dform) {
    add(operand(flds[0], field_get(flds[0], raw), pc, false));
    add(operand(flds[2], field_get(flds[2], raw), pc, false) + "(r" +
        std::to_string(field_get(flds[1], raw)) + ")");
    for (std::size_t i = 3; i < flds.size(); ++i)
      add(operand(flds[i], field_get(flds[i], raw), pc, false));
  } else {
    for (const FieldDesc& f : flds) add(operand(f, field_get(f, raw), pc, absolute));
  }
  return ops.empty() ? mn : mn + " " + ops;
}

}  // namespace gekko
