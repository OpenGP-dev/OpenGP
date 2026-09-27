// SPDX-License-Identifier: GPL-3.0-or-later

#include "analysis.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <deque>
#include <unordered_set>

namespace recomp {

using gekko::Op;
namespace f = gekko::fields;

const char* access_kind_name(AccessKind k) {
  switch (k) {
    case AccessKind::Ram: return "Ram";
    case AccessKind::Mmio: return "Mmio";
    case AccessKind::GatherPipe: return "GatherPipe";
    case AccessKind::Checked: return "Checked";
  }
  return "Checked";
}

AccessKind classify_address(u32 ea) {
  if ((ea >> 16) == 0xCC00) return (ea & 0xFFFFF000) == 0xCC008000 ? AccessKind::GatherPipe : AccessKind::Mmio;
  if ((ea >> 24) == 0xC8) return AccessKind::Mmio;  // the CPU's window onto the EFB
  // MEM1 (up to the 64 MiB Dolphin allows), both views, and the locked cache.
  if ((ea >= 0x80000000 && ea < 0x84000000) || (ea >= 0xC0000000 && ea < 0xC4000000) ||
      (ea >= 0xE0000000 && ea < 0xE0004000))
    return AccessKind::Ram;
  return AccessKind::Checked;
}

std::string sanitize_identifier(const std::string& name, u32 address) {
  static const std::unordered_set<std::string> kReserved = {
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
      "case", "catch", "char", "class", "compl", "concept", "const", "consteval", "constexpr",
      "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
      "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
      "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
      "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
      "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires",
      "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
      "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
      "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
      "while", "xor", "xor_eq", "main", "ctx", "base", "ppc", "guest"};
  char fallback[32];
  std::snprintf(fallback, sizeof(fallback), "fn_%08X", address);
  if (name.empty()) return fallback;

  std::string out;
  out.reserve(name.size());
  for (char ch : name) {
    const unsigned char u = static_cast<unsigned char>(ch);
    out += (std::isalnum(u) || ch == '_') ? ch : '_';
  }
  if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
  // Identifiers with a double underscore or a leading underscore + capital are
  // reserved to the implementation; avoid clashing with anything real.
  const bool reserved = kReserved.count(out) || out.find("__") != std::string::npos ||
                        (out.size() > 1 && out[0] == '_' && std::isupper(static_cast<unsigned char>(out[1])));
  if (reserved) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%08X", address);
    // Collapse double underscores so the result is not itself reserved.
    std::string collapsed;
    for (char ch : out) {
      if (ch == '_' && !collapsed.empty() && collapsed.back() == '_') continue;
      collapsed += ch;
    }
    if (!collapsed.empty() && collapsed[0] == '_') collapsed = "f" + collapsed;
    out = collapsed + suffix;
  }
  return out;
}

// ---- Program --------------------------------------------------------------------------------

bool Program::build(const Dol& dol, const SymbolFile* symbols, const Config& config,
                    std::string* error) {
  dol_ = &dol;
  config_ = &config;
  functions_.clear();
  by_entry_.clear();
  object_sizes_.clear();
  warnings_.clear();

  std::map<u32, Function> fns;
  if (symbols) {
    for (const Symbol& s : symbols->symbols) {
      if (s.type == Symbol::Type::Object && s.size) object_sizes_[s.address] = *s.size;
      if (s.type != Symbol::Type::Function) continue;
      if (!s.size || *s.size == 0) continue;
      if (!dol.is_text(s.address)) {
        warnings_.push_back("function " + s.name + " is not in a text section; skipped");
        continue;
      }
      fns[s.address] = Function{s.address, s.address + *s.size, s.name, {}};
    }
  }
  for (const FunctionOverride& o : config.functions) {
    Function& fn = fns[o.address];
    fn.start = o.address;
    fn.end = o.address + o.size;
    if (!o.name.empty()) fn.symbol = o.name;
  }
  if (fns.empty()) {
    if (error) *error = "no functions: provide a symbols file or [[function]] entries";
    return false;
  }

  std::set<std::string> used_names;
  for (auto& [addr, fn] : fns) {
    if ((fn.start & 3) || (fn.end & 3) || fn.end <= fn.start) {
      warnings_.push_back("function " + fn.symbol + " has a bad range; skipped");
      continue;
    }
    // Trim to the text section it starts in.
    const DolSection* sec = dol.section_at(fn.start, 4);
    if (!sec || !sec->text) {
      warnings_.push_back("function " + fn.symbol + " is not in a text section; skipped");
      continue;
    }
    if (fn.end > sec->address + sec->size) fn.end = sec->address + sec->size;
    // Resolve overlaps with the previous function by truncating it.
    if (!functions_.empty() && functions_.back().end > fn.start) {
      warnings_.push_back("function " + functions_.back().cpp_name + " overlaps " + fn.symbol +
                          "; truncated");
      functions_.back().end = fn.start;
    }
    std::string name = sanitize_identifier(fn.symbol, fn.start);
    if (!used_names.insert(name).second) {
      char suffix[16];
      std::snprintf(suffix, sizeof(suffix), "_%08X", fn.start);
      name += suffix;
      used_names.insert(name);
    }
    fn.cpp_name = name;
    functions_.push_back(fn);
  }
  for (std::size_t i = 0; i < functions_.size(); ++i) by_entry_[functions_[i].start] = i;
  return true;
}

const Function* Program::function_at(u32 entry) const {
  const auto it = by_entry_.find(entry);
  return it == by_entry_.end() ? nullptr : &functions_[it->second];
}

u32 Program::instruction(u32 addr) const {
  if (const auto it = config_->patches.find(addr); it != config_->patches.end()) return it->second;
  u32 v = 0;
  dol_->read_u32(addr, &v);
  return v;
}

std::optional<u32> Program::object_size(u32 addr) const {
  const auto it = object_sizes_.find(addr);
  if (it == object_sizes_.end()) return std::nullopt;
  return it->second;
}

// ---- Function analysis --------------------------------------------------------------------

namespace {

constexpr u32 kVolatileGprs = 0x00001FF9;  // r0, r3-r12

struct RegState {
  std::array<u32, 32> value{};
  u32 known = 0;  // bit n: r<n> holds a known constant

  bool is_known(u32 r) const { return (known >> r) & 1; }
  void set(u32 r, u32 v) {
    value[r] = v;
    known |= 1u << r;
  }
  void clobber(u32 mask) { known &= ~mask; }

  // Meet: keep only constants both states agree on. Returns true if changed.
  bool meet(const RegState& o) {
    u32 agree = known & o.known;
    for (u32 r = 0; r < 32; ++r)
      if ((agree >> r & 1) && value[r] != o.value[r]) agree &= ~(1u << r);
    const bool changed = agree != known;
    known = agree;
    return changed;
  }
};

bool is_call(Op op, u32 raw) {
  switch (op) {
    case Op::b:
    case Op::bc:
    case Op::bclr:
    case Op::bcctr: return f::lk::get(raw) != 0;
    default: return false;
  }
}

bool is_branch(Op op) {
  return op == Op::b || op == Op::bc || op == Op::bclr || op == Op::bcctr || op == Op::rfi;
}

bool branch_always(u32 bo) { return (bo & 0x14) == 0x14; }

void transfer(RegState& s, u32 raw, Op op) {
  const u32 rd = f::rD::get(raw), ra = f::rA::get(raw), rb = f::rB::get(raw);
  switch (op) {
    case Op::addi:
    case Op::addis: {
      const u32 imm = op == Op::addi ? static_cast<u32>(f::simm::get(raw))
                                     : static_cast<u32>(f::simm::get(raw)) << 16;
      if (ra == 0) s.set(rd, imm);
      else if (s.is_known(ra)) s.set(rd, s.value[ra] + imm);
      else s.clobber(1u << rd);
      return;
    }
    case Op::ori:
    case Op::oris:
    case Op::xori:
    case Op::xoris: {
      const u32 rs = rd;  // rS field
      u32 imm = f::uimm::get(raw);
      if (op == Op::oris || op == Op::xoris) imm <<= 16;
      if (s.is_known(rs)) {
        const bool is_or = op == Op::ori || op == Op::oris;
        s.set(ra, is_or ? (s.value[rs] | imm) : (s.value[rs] ^ imm));
      } else {
        s.clobber(1u << ra);
      }
      return;
    }
    case Op::or_: {
      const u32 rs = rd;
      if (s.is_known(rs) && s.is_known(rb)) s.set(ra, s.value[rs] | s.value[rb]);
      else s.clobber(1u << ra);
      return;
    }
    case Op::rlwinm: {
      const u32 rs = rd;
      if (s.is_known(rs)) {
        const u32 sh = f::sh::get(raw), mb = f::mb::get(raw), me = f::me::get(raw);
        const u32 begin = 0xFFFFFFFFu >> mb, end = 0x7FFFFFFFu >> me;
        u32 mask = begin ^ end;
        if (me < mb) mask = ~mask;
        const u32 v = s.value[rs];
        s.set(ra, ((v << sh) | (sh ? v >> (32 - sh) : 0)) & mask);
      } else {
        s.clobber(1u << ra);
      }
      return;
    }
    default:
      break;
  }
  if (is_call(op, raw)) {
    s.clobber(kVolatileGprs);
    return;
  }
  s.clobber(gekko::gpr_writes(raw, op));
}

bool is_stack_or_sda(u32 r) { return r == 1 || r == 2 || r == 13; }

AccessKind classify_access(const RegState& s, u32 raw, Op op) {
  const gekko::OpInfo& info = gekko::op_info(op);
  const bool update = (info.flags & gekko::F_UPD) != 0;
  const u32 ra = f::rA::get(raw), rb = f::rB::get(raw);

  // Base part: rA, or literal 0 when rA == 0 in a non-update form.
  const bool base_zero = ra == 0 && !update;
  const bool base_known = base_zero || s.is_known(ra);
  const u32 base_value = base_zero ? 0 : s.value[ra];

  switch (info.sig) {
    case gekko::Sig::D_LOAD:
    case gekko::Sig::D_STORE:
    case gekko::Sig::D_FLOAD:
    case gekko::Sig::D_FSTORE:
    case gekko::Sig::PSQ_D:
    case gekko::Sig::X_LSWI:
    case gekko::Sig::X_STSWI: {
      s32 disp = 0;
      if (info.sig == gekko::Sig::PSQ_D) disp = f::psq_d::get(raw);
      else if (info.sig != gekko::Sig::X_LSWI && info.sig != gekko::Sig::X_STSWI) disp = f::d::get(raw);
      if (base_known) return classify_address(base_value + static_cast<u32>(disp));
      if (is_stack_or_sda(ra)) return AccessKind::Ram;
      return AccessKind::Checked;
    }
    default: {
      // Indexed forms: EA = (rA|0) + rB.
      if (base_known && s.is_known(rb)) return classify_address(base_value + s.value[rb]);
      if ((!base_zero && is_stack_or_sda(ra)) || is_stack_or_sda(rb)) return AccessKind::Ram;
      // A known RAM address plus an unknown index (array or jump-table access)
      // stays in RAM: reaching the I/O page would need an index near 0x4C000000.
      if ((!base_zero && base_known && classify_address(base_value) == AccessKind::Ram) ||
          (s.is_known(rb) && classify_address(s.value[rb]) == AccessKind::Ram))
        return AccessKind::Ram;
      return AccessKind::Checked;
    }
  }
}

struct Block {
  u32 start = 0;
  u32 end = 0;  // exclusive
  std::vector<u32> successors;
  RegState in;
  bool reached = false;
};

}  // namespace

FunctionAnalysis Program::analyze(const Function& fn) const {
  FunctionAnalysis a;
  a.function = &fn;
  const u32 count = (fn.end - fn.start) / 4;
  a.code.resize(count);
  a.ops.resize(count);
  for (u32 i = 0; i < count; ++i) {
    a.code[i] = instruction(fn.start + 4 * i);
    a.ops[i] = gekko::decode(a.code[i]);
  }
  auto inside = [&](u32 addr) { return addr >= fn.start && addr < fn.end; };
  auto branch_target = [&](u32 pc, u32 raw, Op op) -> u32 {
    const u32 abs = f::aa::get(raw);
    const s32 disp = op == Op::b ? f::li::get(raw) : f::bd::get(raw);
    return (abs ? 0 : pc) + static_cast<u32>(disp);
  };

  RegState entry;
  if (config_->sda_base) entry.set(13, *config_->sda_base);
  if (config_->sda2_base) entry.set(2, *config_->sda2_base);

  // Manual jump tables from the config.
  for (const JumpTableOverride& j : config_->jump_tables) {
    if (!inside(j.bctr)) continue;
    JumpTable t;
    t.table = j.table;
    for (u32 i = 0; i < j.count; ++i) {
      u32 target = 0;
      if (dol_->read_u32(j.table + 4 * i, &target)) t.targets.push_back(target);
    }
    a.jump_tables[j.bctr] = t;
  }

  // Iterate: block construction + dataflow + jump table discovery, until no
  // new jump tables appear (each new table adds labels and edges).
  std::map<u32, Block> blocks;
  for (int round = 0; round < 8; ++round) {
    // Labels: direct branch targets inside the function and jump table targets.
    a.labels.clear();
    for (u32 i = 0; i < count; ++i) {
      const u32 pc = fn.start + 4 * i;
      const Op op = a.ops[i];
      if ((op == Op::b || op == Op::bc) && !(op == Op::b && f::lk::get(a.code[i]))) {
        const u32 t = branch_target(pc, a.code[i], op);
        if (inside(t) && !f::lk::get(a.code[i])) a.labels.insert(t);
      }
    }
    for (const auto& [pc, jt] : a.jump_tables)
      for (u32 t : jt.targets)
        if (inside(t)) a.labels.insert(t);

    // Blocks.
    std::set<u32> leaders = {fn.start};
    for (u32 l : a.labels) leaders.insert(l);
    for (u32 i = 0; i < count; ++i) {
      const Op op = a.ops[i];
      if ((is_branch(op) || op == Op::sc) && i + 1 < count) leaders.insert(fn.start + 4 * (i + 1));
    }
    blocks.clear();
    for (auto it = leaders.begin(); it != leaders.end(); ++it) {
      Block b;
      b.start = *it;
      auto next = std::next(it);
      b.end = next == leaders.end() ? fn.end : *next;
      blocks[b.start] = b;
    }
    for (auto& [start, b] : blocks) {
      const u32 last = b.end - 4;
      const u32 raw = a.word(last);
      const Op op = a.op(last);
      const bool lk = is_branch(op) && op != Op::rfi && f::lk::get(raw);
      auto add = [&](u32 t) {
        if (inside(t)) b.successors.push_back(t);
      };
      switch (op) {
        case Op::b:
          if (lk) add(b.end);
          else add(branch_target(last, raw, op));
          break;
        case Op::bc:
          if (lk) {
            add(b.end);
          } else {
            add(branch_target(last, raw, op));
            if (!branch_always(f::bo::get(raw))) add(b.end);
          }
          break;
        case Op::bclr:
          if (lk || !branch_always(f::bo::get(raw))) add(b.end);
          break;
        case Op::bcctr:
          if (lk) {
            add(b.end);
          } else {
            if (const auto jt = a.jump_tables.find(last); jt != a.jump_tables.end())
              for (u32 t : jt->second.targets) add(t);
            if (!branch_always(f::bo::get(raw))) add(b.end);
          }
          break;
        case Op::rfi:
          break;
        default:
          add(b.end);
          break;
      }
    }

    // Dataflow to a fixpoint.
    std::deque<u32> work = {fn.start};
    blocks[fn.start].in = entry;
    blocks[fn.start].reached = true;
    while (!work.empty()) {
      Block& b = blocks[work.front()];
      work.pop_front();
      RegState s = b.in;
      for (u32 pc = b.start; pc < b.end; pc += 4) transfer(s, a.word(pc), a.op(pc));
      for (u32 succ : b.successors) {
        Block& sb = blocks[succ];
        if (!sb.reached) {
          sb.reached = true;
          sb.in = s;
          work.push_back(succ);
        } else if (sb.in.meet(s)) {
          work.push_back(succ);
        }
      }
    }
    // Blocks not reached from the entry (only reachable through code we could
    // not resolve) get the conservative entry state.
    for (auto& [start, b] : blocks)
      if (!b.reached) b.in = entry;

    // Jump table discovery for unresolved bctr.
    bool found_new = false;
    for (auto& [start, b] : blocks) {
      const u32 last = b.end - 4;
      const u32 raw = a.word(last);
      if (a.op(last) != Op::bcctr || f::lk::get(raw) || a.jump_tables.count(last)) continue;

      // Replay the block to get the state before each instruction.
      std::vector<RegState> before;
      RegState s = b.in;
      for (u32 pc = b.start; pc < b.end; pc += 4) {
        before.push_back(s);
        transfer(s, a.word(pc), a.op(pc));
      }
      // mtctr rX ... lwzx rX, rA, rB with rA a known table address.
      std::optional<u32> ctr_reg;
      u32 mtctr_pc = 0;
      for (u32 pc = last; pc > b.start;) {
        pc -= 4;
        if (a.op(pc) == Op::mtspr && f::spr::get(a.word(pc)) == 9) {
          ctr_reg = f::rS::get(a.word(pc));
          mtctr_pc = pc;
          break;
        }
      }
      if (!ctr_reg) continue;
      for (u32 pc = mtctr_pc; pc > b.start;) {
        pc -= 4;
        const u32 w = a.word(pc);
        if (!(gekko::gpr_writes(w, a.op(pc)) & (1u << *ctr_reg))) continue;
        if (a.op(pc) == Op::lwzx && f::rD::get(w) == *ctr_reg) {
          const RegState& st = before[(pc - b.start) / 4];
          const u32 ra = f::rA::get(w), rb = f::rB::get(w);
          std::optional<u32> table;
          if (ra != 0 && st.is_known(ra)) table = st.value[ra];
          else if (st.is_known(rb)) table = st.value[rb];
          if (table) {
            JumpTable jt;
            jt.table = *table;
            const auto size = object_size(*table);
            const u32 max_entries = size ? *size / 4 : 1024;
            for (u32 i = 0; i < max_entries; ++i) {
              u32 target = 0;
              if (!dol_->read_u32(*table + 4 * i, &target)) break;
              if (!size && (!inside(target) || (target & 3))) break;
              jt.targets.push_back(target);
            }
            if (!jt.targets.empty()) {
              a.jump_tables[last] = jt;
              found_new = true;
            }
          }
        }
        break;
      }
    }
    if (!found_new) break;
  }

  // Classify every memory access using the state before it.
  for (auto& [start, b] : blocks) {
    RegState s = b.in;
    for (u32 pc = b.start; pc < b.end; pc += 4) {
      const u32 raw = a.word(pc);
      const Op op = a.op(pc);
      if (gekko::op_info(op).cc == gekko::CallConv::MEM) a.accesses[pc] = classify_access(s, raw, op);
      transfer(s, raw, op);
    }
  }

  // Report bctr that stayed unresolved.
  for (u32 i = 0; i < count; ++i) {
    const u32 pc = fn.start + 4 * i;
    if (a.ops[i] == Op::bcctr && !f::lk::get(a.code[i]) && !a.jump_tables.count(pc)) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "unresolved bctr at %08X (dispatched at runtime)", pc);
      a.warnings.push_back(buf);
    }
    if (a.ops[i] == Op::Invalid) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "invalid instruction %08X at %08X", a.code[i], pc);
      a.warnings.push_back(buf);
    }
  }
  return a;
}

}  // namespace recomp
