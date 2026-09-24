// SPDX-License-Identifier: GPL-3.0-or-later

#include "gekko/isa.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace gekko {

namespace {

constexpr OpInfo kOps[] = {
    {Op::Invalid, "invalid", "<invalid>", 0, 0, XoKind::PRI, Sig::NONE, CallConv::CTX, 0},
#define GEKKO_OP(id_, mn, opcd_, xo_, kind_, sig_, cc_, flags_)                                  \
  {Op::id_, #id_, mn, opcd_, xo_, XoKind::kind_, Sig::sig_, CallConv::cc_, static_cast<u32>(flags_)},
#include "gekko/ops.inc"
#undef GEKKO_OP
};
static_assert(std::size(kOps) == static_cast<std::size_t>(Op::Count));

struct SigEntry {
  const char* name;
  const FieldDesc* fields;
  std::size_t count;
};

constexpr SigEntry kSigs[] = {
#define GEKKO_SIG(name, ...) {#name, sigs::name::descs.data(), sigs::name::count},
#include "gekko/sigs.inc"
#undef GEKKO_SIG
};

// Decoder tables, built once.
struct Tables {
  std::array<Op, 64> primary{};
  std::array<Op, 1024> op4_x10{}, op19{}, op31{}, op63_x10{};
  std::array<Op, 32> op4_x5{}, op59{}, op63_x5{};
  std::array<Op, 64> op4_x6{};

  Tables() {
    for (const OpInfo& info : kOps) {
      if (info.op == Op::Invalid) continue;
      switch (info.kind) {
        case XoKind::PRI:
          primary[info.opcd] = info.op;
          break;
        case XoKind::X9:
          // XO-form: the OE bit is part of the 10-bit field; both values decode.
          op31[info.xo] = info.op;
          op31[info.xo | 0x200] = info.op;
          break;
        case XoKind::X10:
          switch (info.opcd) {
            case 4: op4_x10[info.xo] = info.op; break;
            case 19: op19[info.xo] = info.op; break;
            case 31: op31[info.xo] = info.op; break;
            case 63: op63_x10[info.xo] = info.op; break;
            default: throw std::logic_error("bad X10 opcode");
          }
          break;
        case XoKind::X5:
          switch (info.opcd) {
            case 4: op4_x5[info.xo] = info.op; break;
            case 59: op59[info.xo] = info.op; break;
            case 63: op63_x5[info.xo] = info.op; break;
            default: throw std::logic_error("bad X5 opcode");
          }
          break;
        case XoKind::X6:
          op4_x6[info.xo] = info.op;
          break;
      }
    }
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

const OpInfo& op_info(Op op) {
  const auto i = static_cast<std::size_t>(op);
  return i < std::size(kOps) ? kOps[i] : kOps[0];
}

std::span<const OpInfo> all_ops() { return {kOps + 1, std::size(kOps) - 1}; }

std::span<const FieldDesc> sig_fields(Sig sig) {
  const SigEntry& e = kSigs[static_cast<std::size_t>(sig)];
  return {e.fields, e.count};
}

const char* sig_name(Sig sig) { return kSigs[static_cast<std::size_t>(sig)].name; }

Op decode(u32 raw) {
  const Tables& t = tables();
  const u32 opcd = raw >> 26;
  const u32 x10 = (raw >> 1) & 0x3FF;
  const u32 x5 = (raw >> 1) & 0x1F;
  switch (opcd) {
    case 4: {
      if (Op op = t.op4_x5[x5]; op != Op::Invalid) return op;
      if (Op op = t.op4_x6[(raw >> 1) & 0x3F]; op != Op::Invalid) return op;
      return t.op4_x10[x10];
    }
    case 19: return t.op19[x10];
    case 31: return t.op31[x10];
    case 59: return t.op59[x5];
    case 63: {
      if (Op op = t.op63_x5[x5]; op != Op::Invalid) return op;
      return t.op63_x10[x10];
    }
    default: return t.primary[opcd];
  }
}

s64 field_get(const FieldDesc& f, u32 raw) {
  switch (f.kind) {
    case FieldKind::Spr: return fields::spr::get(raw);
    case FieldKind::BranchTarget:
      return f.width == 24 ? fields::li::get(raw) : fields::bd::get(raw);
    default: break;
  }
  const u32 bits = (raw >> f.shift) & ((1u << f.width) - 1);
  if (f.is_signed) return fields::sign_extend(bits, f.width);
  return bits;
}

u32 field_put(const FieldDesc& f, s64 value) {
  const u32 v = static_cast<u32>(value);
  switch (f.kind) {
    case FieldKind::Spr: return ((v & 31) << 16) | (((v >> 5) & 31) << 11);
    case FieldKind::BranchTarget:
      return f.width == 24 ? (v & 0x03FFFFFC) : (v & 0xFFFC);
    default: break;
  }
  return (v & ((1u << f.width) - 1)) << f.shift;
}

u32 encode(Op op, std::span<const s64> values) {
  const OpInfo& info = op_info(op);
  if (op == Op::Invalid) throw std::invalid_argument("encode: invalid op");
  const auto flds = sig_fields(info.sig);
  if (values.size() != flds.size())
    throw std::invalid_argument(std::string("encode: wrong field count for ") + info.mnemonic);

  u32 raw = static_cast<u32>(info.opcd) << 26;
  switch (info.kind) {
    case XoKind::PRI: break;
    case XoKind::X10:
    case XoKind::X9: raw |= static_cast<u32>(info.xo) << 1; break;
    case XoKind::X5: raw |= static_cast<u32>(info.xo) << 1; break;
    case XoKind::X6: raw |= static_cast<u32>(info.xo) << 1; break;
  }
  for (std::size_t i = 0; i < flds.size(); ++i) raw |= field_put(flds[i], values[i]);
  return raw;
}

u32 encode(Op op, std::initializer_list<s64> values) {
  return encode(op, std::span<const s64>(values.begin(), values.size()));
}

u32 gpr_writes(u32 raw, Op op) {
  const OpInfo& info = op_info(op);
  const u32 rd = fields::rD::get(raw);
  const u32 ra = fields::rA::get(raw);
  u32 mask = 0;
  if (info.flags & F_UPD) mask |= 1u << ra;
  switch (op) {
    case Op::lmw:
      for (u32 r = rd; r < 32; ++r) mask |= 1u << r;
      return mask;
    case Op::lswi:
    case Op::lswx:
      return 0xFFFFFFFFu;  // conservatively: any register
    case Op::stwcx_rc:
      return mask;
    default:
      break;
  }
  switch (info.sig) {
    case Sig::D_rD_rA_SIMM:
    case Sig::XO_rD_rA_rB:
    case Sig::XO_rD_rA:
    case Sig::X_rD_rA_rB_RC:
    case Sig::X_rD:
    case Sig::X_MFSPR:
    case Sig::X_MFSR:
    case Sig::X_MFSRIN:
      mask |= 1u << rd;
      break;
    case Sig::D_rA_rS_UIMM:
    case Sig::X_rA_rS_rB:
    case Sig::X_rA_rS:
    case Sig::X_SRAWI:
    case Sig::M_RLWI:
    case Sig::M_RLWNM:
      mask |= 1u << ra;
      break;
    case Sig::D_LOAD:
    case Sig::X_LOADX:
      if (info.flags & F_LOAD) mask |= 1u << rd;
      break;
    default:
      break;
  }
  return mask;
}

}  // namespace gekko
