// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generates the differential-test fixture: a DOL containing hand-written
// functions (covering every control-flow shape the recompiler handles) and
// randomly generated straight-line functions (covering instruction
// semantics plumbing), plus the matching symbols.txt, recomp.toml and a list
// of test cases.
//
//   fixturegen <output-dir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "dol.h"
#include "gekko/isa.h"
#include "symbols.h"

using namespace gekko;

namespace {

constexpr u32 kText = 0x80003100;
constexpr u32 kData = 0x80200000;
constexpr u32 kScratch = 0x80300000;
constexpr u32 kSdaBase = kScratch + 0x8000;   // r13
constexpr u32 kSda2Base = kScratch + 0x4000;  // r2
constexpr int kRandomFunctions = 160;

u32 ha(u32 a) { return ((a + 0x8000) >> 16) & 0xFFFF; }
s32 lo(u32 a) { return static_cast<s16>(a & 0xFFFF); }

// Minimal assembler with labels.
class Asm {
 public:
  u32 pc() const { return kText + 4 * static_cast<u32>(words_.size()); }
  void op(Op o, std::initializer_list<s64> f) { words_.push_back(encode(o, f)); }
  void word(u32 w) { words_.push_back(w); }
  void label(const std::string& name) { labels_[name] = pc(); }
  u32 address_of(const std::string& name) const { return labels_.at(name); }
  bool has_label(const std::string& name) const { return labels_.count(name) != 0; }

  void b(const std::string& target, bool link = false) {
    fixups_.push_back({words_.size(), target, Op::b, 0, 0, link});
    words_.push_back(0);
  }
  void bc(u32 bo, u32 bi, const std::string& target, bool link = false) {
    fixups_.push_back({words_.size(), target, Op::bc, bo, bi, link});
    words_.push_back(0);
  }
  void blr() { op(Op::bclr, {20, 0, 0}); }
  void mr(u32 d, u32 s) { op(Op::or_, {d, s, s, 0}); }
  void li(u32 d, s32 v) { op(Op::addi, {d, 0, v}); }
  void lis(u32 d, u32 v) { op(Op::addis, {d, 0, static_cast<s16>(v)}); }
  void mflr(u32 d) { op(Op::mfspr, {d, 8}); }
  void mtlr(u32 s) { op(Op::mtspr, {8, s}); }
  void mtctr(u32 s) { op(Op::mtspr, {9, s}); }

  void resolve() {
    for (const Fixup& f : fixups_) {
      const u32 at = kText + 4 * static_cast<u32>(f.index);
      const s64 disp = static_cast<s64>(labels_.at(f.target)) - at;
      words_[f.index] = f.op == Op::b ? encode(Op::b, {disp, 0, f.link})
                                       : encode(Op::bc, {f.bo, f.bi, disp, 0, f.link});
    }
  }
  std::vector<u8> bytes() const {
    std::vector<u8> out;
    for (u32 w : words_)
      for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<u8>(w >> s));
    return out;
  }

 private:
  struct Fixup {
    std::size_t index;
    std::string target;
    Op op;
    u32 bo, bi;
    bool link;
  };
  std::vector<u32> words_;
  std::map<std::string, u32> labels_;
  std::vector<Fixup> fixups_;
};

struct FunctionInfo {
  std::string name;
  u32 start = 0, end = 0;
  bool in_symbols = true;
};

struct CaseInfo {
  std::string name;
  int kind;     // 0 = hand-written diff, 1 = random diff, 2 = recompiled only
  u32 arg_mod;  // if non-zero, r3 is reduced modulo this
};

class Fixture {
 public:
  Asm a;
  std::vector<FunctionInfo> functions;
  std::vector<CaseInfo> cases;
  std::vector<std::pair<std::string, u32>> data_words;  // (label or "", value) laid out at kData
  std::vector<std::string> data_labels_at;              // label for each data word (or "")
  std::map<std::string, u32> data_symbols;              // name -> offset of object start
  std::map<std::string, u32> data_sizes;
  std::string config_extra;

  void begin(const std::string& name, bool in_symbols = true) {
    a.label(name);
    functions.push_back({name, a.pc(), 0, in_symbols});
  }
  void end() { functions.back().end = a.pc(); }
  void test(const std::string& name, int kind = 0, u32 arg_mod = 0) { cases.push_back({name, kind, arg_mod}); }

  // Data: words that may reference code labels (resolved after assembly).
  u32 data_object(const std::string& name, const std::vector<std::string>& label_refs, bool symbol = true) {
    const u32 offset = static_cast<u32>(data_words.size()) * 4;
    for (const std::string& ref : label_refs) data_words.push_back({ref, 0});
    if (symbol) {
      data_symbols[name] = offset;
      data_sizes[name] = static_cast<u32>(label_refs.size()) * 4;
    }
    named_[name] = kData + offset;
    return kData + offset;
  }
  u32 data_value(const std::string& name, u32 value) {
    const u32 offset = static_cast<u32>(data_words.size()) * 4;
    data_words.push_back({"", value});
    named_[name] = kData + offset;
    return kData + offset;
  }
  u32 addr(const std::string& data_name) const { return named_.at(data_name); }

 private:
  std::map<std::string, u32> named_;
};

// ---- Hand-written functions ---------------------------------------------------------------

void hand_written(Fixture& fx) {
  Asm& a = fx.a;

  // Data objects (addresses are fixed; contents resolved after assembly).
  fx.data_object("jumptable_switch", {"sw_case0", "sw_case1", "sw_case2", "sw_case3", "sw_case4"});
  fx.data_value("jt_switch_end", 0);
  fx.data_object("jt_heuristic", {"sh_case0", "sh_case1", "sh_case2"}, /*symbol=*/false);
  fx.data_value("jt_heuristic_end", 0);
  fx.data_object("jt_config", {"sc_case0", "sc_case1", "sc_case2", "sc_case3"}, false);
  fx.data_value("jt_config_ptr", 0);  // patched below to point at jt_config
  fx.data_object("fptr_table", {"helper_add", "helper_mul"});
  fx.data_value("gp_pointer", 0xCC008000);

  // helper_add: r3 += r4
  fx.begin("helper_add");
  a.op(Op::add, {3, 3, 4, 0, 0});
  a.blr();
  fx.end();

  // helper_mul: r3 *= r4
  fx.begin("helper_mul");
  a.op(Op::mullw, {3, 3, 4, 0, 0});
  a.blr();
  fx.end();

  // Direct call with a stack frame and LR save/restore.
  fx.begin("fn_call_chain");
  a.op(Op::stwu, {1, 1, -32});
  a.mflr(0);
  a.op(Op::stw, {0, 1, 36});
  a.op(Op::stw, {31, 1, 28});
  a.mr(31, 3);
  a.li(4, 5);
  a.b("helper_add", true);
  a.op(Op::add, {3, 3, 31, 0, 0});
  a.op(Op::lwz, {0, 1, 36});
  a.op(Op::lwz, {31, 1, 28});
  a.mtlr(0);
  a.op(Op::addi, {1, 1, 32});
  a.blr();
  fx.end();
  fx.test("fn_call_chain");

  // CTR loop with an update-form load (backward branch).
  fx.begin("fn_loop_sum");
  a.li(3, 0);
  a.li(4, 16);
  a.mtctr(4);
  a.op(Op::addi, {5, 31, -4});
  a.label("ls_loop");
  a.op(Op::lwzu, {6, 5, 4});
  a.op(Op::add, {3, 3, 6, 0, 0});
  a.bc(16, 0, "ls_loop");
  a.blr();
  fx.end();
  fx.test("fn_loop_sum");

  // switch through a jump table sized by a symbol.
  const u32 jt = fx.addr("jumptable_switch");
  fx.begin("fn_switch");
  a.op(Op::cmpli, {0, 3, 4});
  a.bc(12, 1, "sw_default");  // bgt
  a.lis(5, ha(jt));
  a.op(Op::rlwinm, {0, 3, 2, 0, 29, 0});
  a.op(Op::addi, {5, 5, lo(jt)});
  a.op(Op::lwzx, {0, 5, 0});
  a.mtctr(0);
  a.op(Op::bcctr, {20, 0, 0});
  for (int i = 0; i < 5; ++i) {
    a.label("sw_case" + std::to_string(i));
    a.li(4, 10 * (i + 1));
    a.b("sw_end");
  }
  a.label("sw_default");
  a.li(4, -1);
  a.label("sw_end");
  a.blr();
  fx.end();
  fx.test("fn_switch", 0, 7);

  // switch whose table has no symbol: found by scanning.
  const u32 jth = fx.addr("jt_heuristic");
  fx.begin("fn_switch_heuristic");
  a.op(Op::cmpli, {0, 3, 2});
  a.bc(12, 1, "sh_default");
  a.lis(5, ha(jth));
  a.op(Op::addi, {5, 5, lo(jth)});
  a.op(Op::rlwinm, {6, 3, 2, 0, 29, 0});
  a.op(Op::lwzx, {6, 5, 6});
  a.mtctr(6);
  a.op(Op::bcctr, {20, 0, 0});
  for (int i = 0; i < 3; ++i) {
    a.label("sh_case" + std::to_string(i));
    a.op(Op::addi, {3, 3, 100 * (i + 1)});
    a.blr();
  }
  a.label("sh_default");
  a.li(3, 0);
  a.blr();
  fx.end();
  fx.test("fn_switch_heuristic", 0, 5);

  // switch whose table address is loaded from memory: given in the config.
  const u32 jtp = fx.addr("jt_config_ptr");
  fx.begin("fn_switch_config");
  a.op(Op::cmpli, {0, 3, 3});
  a.bc(12, 1, "sc_default");
  a.lis(5, ha(jtp));
  a.op(Op::lwz, {5, 5, lo(jtp)});
  a.op(Op::rlwinm, {6, 3, 2, 0, 29, 0});
  a.op(Op::lwzx, {6, 5, 6});
  a.mtctr(6);
  const u32 bctr_at = a.pc();
  a.op(Op::bcctr, {20, 0, 0});
  for (int i = 0; i < 4; ++i) {
    a.label("sc_case" + std::to_string(i));
    a.op(Op::xori, {3, 3, static_cast<u32>(0x1111 * (i + 1))});
    a.blr();
  }
  a.label("sc_default");
  a.li(3, 1);
  a.blr();
  fx.end();
  fx.test("fn_switch_config", 0, 6);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "\n[[jump_table]]\nbctr = 0x%08X\ntable = 0x%08X\ncount = 4\n", bctr_at,
                fx.addr("jt_config"));
  fx.config_extra += buf;

  // Indirect call through a function pointer table (bctrl).
  const u32 fpt = fx.addr("fptr_table");
  fx.begin("fn_indirect_call");
  a.op(Op::stwu, {1, 1, -16});
  a.mflr(0);
  a.op(Op::stw, {0, 1, 20});
  a.lis(5, ha(fpt));
  a.op(Op::addi, {5, 5, lo(fpt)});
  a.op(Op::rlwinm, {6, 3, 2, 29, 29, 0});  // (r3 & 1) * 4
  a.op(Op::lwzx, {0, 5, 6});
  a.mtctr(0);
  a.li(4, 3);
  a.op(Op::bcctr, {20, 0, 1});
  a.op(Op::lwz, {0, 1, 20});
  a.mtlr(0);
  a.op(Op::addi, {1, 1, 16});
  a.blr();
  fx.end();
  fx.test("fn_indirect_call");

  // Recursion: fib(n), n = r3 % 12.
  fx.begin("fn_fib");
  a.op(Op::cmpi, {0, 3, 2});
  a.bc(12, 0, "fib_small");
  a.op(Op::stwu, {1, 1, -16});
  a.mflr(0);
  a.op(Op::stw, {0, 1, 20});
  a.op(Op::stw, {30, 1, 8});
  a.op(Op::stw, {29, 1, 12});
  a.mr(30, 3);
  a.op(Op::addi, {3, 3, -1});
  a.b("fn_fib", true);
  a.mr(29, 3);
  a.op(Op::addi, {3, 30, -2});
  a.b("fn_fib", true);
  a.op(Op::add, {3, 3, 29, 0, 0});
  a.op(Op::lwz, {29, 1, 12});
  a.op(Op::lwz, {30, 1, 8});
  a.op(Op::lwz, {0, 1, 20});
  a.mtlr(0);
  a.op(Op::addi, {1, 1, 16});
  a.label("fib_small");
  a.blr();
  fx.end();
  fx.test("fn_fib", 0, 12);

  // Tail call into another function.
  fx.begin("fn_tail_call");
  a.op(Op::addi, {3, 3, 100});
  a.b("helper_add");
  fx.end();
  fx.test("fn_tail_call");

  // Function that falls through into the next one (like _savegpr_N chains).
  fx.begin("fn_ft_a");
  a.op(Op::addi, {3, 3, 1});
  fx.end();
  fx.begin("fn_ft_b");
  a.op(Op::addi, {3, 3, 2});
  a.blr();
  fx.end();
  fx.test("fn_ft_a");

  // Write-gather pipe: address built with lis, used inside a loop.
  fx.begin("fn_gather");
  a.lis(3, 0xCC01);
  a.li(4, 3);
  a.mtctr(4);
  a.label("gp_loop");
  a.op(Op::stw, {5, 3, -0x8000});
  a.op(Op::stfs, {1, 3, -0x8000});
  a.op(Op::psq_st, {2, 3, -0x800, 0, 0});  // 12-bit displacement: 0xCC010000 - 0x800 is not the pipe
  a.op(Op::addi, {7, 3, -0x8000});
  a.op(Op::psq_st, {2, 7, 0, 0, 1});
  a.op(Op::stb, {6, 3, -0x8000});
  a.bc(16, 0, "gp_loop");
  a.blr();
  fx.end();
  fx.test("fn_gather");

  // Hardware register reads and writes.
  fx.begin("fn_mmio");
  a.lis(3, 0xCC00);
  a.op(Op::lhz, {4, 3, 0x2002});
  a.op(Op::lwz, {5, 3, 0x6000});
  a.op(Op::stw, {5, 3, 0x3000});
  a.op(Op::sth, {4, 3, 0x2004});
  a.blr();
  fx.end();
  fx.test("fn_mmio");

  // Pointer loaded from memory: the recompiler cannot tell, so checks at runtime.
  const u32 gpp = fx.addr("gp_pointer");
  fx.begin("fn_checked");
  a.lis(3, ha(gpp));
  a.op(Op::lwz, {3, 3, lo(gpp)});
  a.op(Op::stw, {5, 3, 0});
  a.mr(8, 31);
  a.op(Op::stw, {6, 8, 0x40});
  a.op(Op::lwz, {9, 8, 0x80});
  a.blr();
  fx.end();
  fx.test("fn_checked");

  // Conditional returns.
  fx.begin("fn_condret");
  a.op(Op::cmpi, {0, 3, 0});
  a.op(Op::bclr, {12, 2, 0});  // beqlr
  a.li(4, 7);
  a.op(Op::cmpli, {3, 3, 1});
  a.op(Op::bclr, {12, 13, 0});  // bgtlr cr3
  a.li(4, 9);
  a.blr();
  fx.end();
  fx.test("fn_condret", 0, 3);

  // bl to the next instruction to read the PC.
  fx.begin("fn_bl_next");
  a.mflr(0);
  a.b("bn_next", true);
  a.label("bn_next");
  a.mflr(3);
  a.mtlr(0);
  a.blr();
  fx.end();
  fx.test("fn_bl_next");

  // String loads/stores.
  fx.begin("fn_strings");
  a.op(Op::lswi, {5, 31, 7});
  a.op(Op::addi, {29, 0, 16});
  a.li(4, 9);
  a.op(Op::mtspr, {1, 4});
  a.op(Op::lswx, {7, 31, 29});
  a.op(Op::addi, {29, 0, 64});
  a.op(Op::stswx, {7, 31, 29});
  a.op(Op::addi, {10, 31, 96});
  a.op(Op::stswi, {5, 10, 7});
  a.li(29, 0);
  a.blr();
  fx.end();
  fx.test("fn_strings");

  // Condition register logic feeding a branch.
  fx.begin("fn_crlogic");
  a.op(Op::cmp, {0, 3, 4});
  a.op(Op::cmpi, {1, 5, 0});
  a.op(Op::crxor, {2, 2, 6});
  a.li(6, 0);
  a.bc(12, 2, "cr_skip");
  a.li(6, 1);
  a.label("cr_skip");
  a.op(Op::mfcr, {7});
  a.blr();
  fx.end();
  fx.test("fn_crlogic", 0, 2);

  // Branch into the middle of another function: handled by the interpreter.
  fx.begin("fn_jump_middle");
  a.op(Op::addi, {3, 3, 1});
  a.b("jm_mid");
  fx.end();
  fx.begin("fn_jump_target");
  a.li(3, 0);
  a.label("jm_mid");
  a.op(Op::addi, {3, 3, 10});
  a.blr();
  fx.end();
  fx.test("fn_jump_middle");

  // Tail call through a pointer loaded with lwz (unresolvable bctr).
  fx.begin("fn_ptr_tail");
  a.lis(5, ha(fpt));
  a.op(Op::lwz, {0, 5, lo(fpt)});
  a.mtctr(0);
  a.li(4, 11);
  a.op(Op::bcctr, {20, 0, 0});
  fx.end();
  fx.test("fn_ptr_tail");

  // Instruction patched by the config: li r3, 1 -> li r3, 2.
  fx.begin("fn_patched");
  const u32 patch_at = a.pc();
  a.li(3, 1);
  a.blr();
  fx.end();
  fx.test("fn_patched");
  std::snprintf(buf, sizeof(buf), "\n[[patch]]\naddress = 0x%08X\nvalue = 0x%08X\n", patch_at,
                encode(Op::addi, {3, 0, 2}));
  fx.config_extra += buf;

  // Mid-function hook replacing an instruction (recompiled code only).
  fx.begin("fn_hooked");
  a.li(5, 0);
  const u32 hook_at = a.pc();
  a.li(6, 0);
  a.blr();
  fx.end();
  fx.test("fn_hooked", 2);
  std::snprintf(buf, sizeof(buf), "\n[[hook]]\naddress = 0x%08X\nfunction = \"TestHook\"\nskip = true\n",
                hook_at);
  fx.config_extra += buf;

  // Function known only from the config, not the symbol file.
  fx.begin("fn_config_only", /*in_symbols=*/false);
  a.op(Op::addi, {3, 3, 7});
  a.blr();
  fx.end();
  fx.test("fn_config_only");
  std::snprintf(buf, sizeof(buf), "\n[[function]]\naddress = 0x%08X\nsize = 8\nname = \"fn_config_only\"\n",
                fx.functions.back().start);
  fx.config_extra += buf;

  // stmw/lmw register save and restore.
  fx.begin("fn_lmw_stmw");
  a.op(Op::stwu, {1, 1, -48});
  a.op(Op::stmw, {27, 1, 20});
  for (u32 r = 27; r < 32; ++r) a.op(Op::addi, {r, 3, static_cast<s32>(r)});
  a.op(Op::add, {3, 27, 31, 0, 0});
  a.op(Op::lmw, {27, 1, 20});
  a.op(Op::addi, {1, 1, 48});
  a.blr();
  fx.end();
  fx.test("fn_lmw_stmw");

  // Small-data accesses through r13 and r2.
  fx.begin("fn_sda");
  a.op(Op::lwz, {3, 13, 0x10});
  a.op(Op::addi, {3, 3, 1});
  a.op(Op::stw, {3, 2, 0x20});
  a.op(Op::lfd, {1, 13, -0x40});
  a.op(Op::stfd, {1, 2, 0x28});
  a.blr();
  fx.end();
  fx.test("fn_sda");

  // Locked cache DMA and dcbz_l.
  fx.begin("fn_locked_cache");
  a.op(Op::addis, {5, 0, static_cast<s16>((kScratch & 0x1FFFFFFF) >> 16)});
  a.op(Op::mtspr, {922, 5});                       // DMA_U: scratch, length high 0
  a.lis(6, 0xE000);
  a.op(Op::ori, {6, 6, 0x10 | (1 << 2) | 2});      // load, 1 line, trigger
  a.op(Op::mtspr, {923, 6});
  a.op(Op::lwz, {3, 6, -0x16});                    // reads the locked cache (0xE0000000)
  a.li(7, 32);
  a.op(Op::dcbz_l, {6, 7});                        // zero LC line 1 (0xE0000020)
  a.op(Op::lwz, {4, 6, 0x0A});                     // 0xE0000020
  a.blr();
  fx.end();
  fx.test("fn_locked_cache");
}

// ---- Random functions ---------------------------------------------------------------------

class RandomGen {
 public:
  explicit RandomGen(u32 seed) : rng_(seed) {}

  u32 range(u32 lo, u32 hi) { return std::uniform_int_distribution<u32>(lo, hi)(rng_); }
  bool chance(u32 percent) { return range(0, 99) < percent; }
  s32 simm() { return static_cast<s16>(range(0, 0xFFFF)); }

  // Destination GPRs never include r1/r2/r13 (stack/SDA) or r29-r31 (memory bases).
  u32 dst() {
    static const u32 kRegs[] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20,
                                21, 22, 23, 24, 25, 26, 27, 28};
    return kRegs[range(0, std::size(kRegs) - 1)];
  }
  u32 src() { return range(0, 31); }
  u32 fpr() { return range(0, 31); }
  u32 rc() { return chance(30) ? 1 : 0; }

  void gen_function(Asm& a, int length) {
    std::vector<std::string> pending_labels;
    int label_id = 0;
    const std::string prefix = "r" + std::to_string(fn_counter_++) + "_";
    for (int i = 0; i < length; ++i) {
      // Place labels that forward branches were aimed at.
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second <= i) {
          a.label(it->first);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
      if (chance(6) && i + 2 < length) {
        static const u32 kBo[] = {4, 12, 20, 16, 18, 0, 2, 8, 10, 5, 13};
        const std::string lbl = prefix + std::to_string(label_id++);
        pending_[lbl] = i + static_cast<int>(range(1, 6));
        a.bc(kBo[range(0, std::size(kBo) - 1)], range(0, 31), lbl);
        continue;
      }
      gen_instruction(a);
    }
    for (const auto& [lbl, at] : pending_) a.label(lbl);
    pending_.clear();
    a.blr();
  }

 private:
  void gen_instruction(Asm& a) {
    switch (range(0, 9)) {
      case 0:
      case 1:
      case 2: gen_integer(a); return;
      case 3: gen_cr(a); return;
      case 4:
      case 5:
      case 6: gen_fp(a); return;
      case 7:
      case 8: gen_memory(a); return;
      default: gen_spr(a); return;
    }
  }

  void gen_integer(Asm& a) {
    static const Op kImmD[] = {Op::addi, Op::addis, Op::addic, Op::addic_rc, Op::mulli, Op::subfic};
    static const Op kImmA[] = {Op::ori, Op::oris, Op::xori, Op::xoris, Op::andi_rc, Op::andis_rc};
    static const Op kXO[] = {Op::add, Op::addc, Op::adde, Op::subf, Op::subfc, Op::subfe,
                             Op::mullw, Op::divw, Op::divwu};
    static const Op kXO1[] = {Op::neg, Op::addze, Op::addme, Op::subfze, Op::subfme};
    static const Op kLogic[] = {Op::and_, Op::andc, Op::or_, Op::orc, Op::xor_, Op::nor,
                                Op::nand, Op::eqv, Op::slw, Op::srw, Op::sraw};
    static const Op kUnary[] = {Op::cntlzw, Op::extsb, Op::extsh};
    switch (range(0, 9)) {
      case 0: a.op(pick(kImmD), {dst(), src(), simm()}); return;
      case 1: a.op(pick(kImmA), {dst(), src(), range(0, 0xFFFF)}); return;
      case 2: a.op(pick(kXO), {dst(), src(), src(), range(0, 1), rc()}); return;
      case 3: a.op(pick(kXO1), {dst(), src(), range(0, 1), rc()}); return;
      case 4: a.op(chance(50) ? Op::mulhw : Op::mulhwu, {dst(), src(), src(), rc()}); return;
      case 5: a.op(pick(kLogic), {dst(), src(), src(), rc()}); return;
      case 6: a.op(pick(kUnary), {dst(), src(), rc()}); return;
      case 7: a.op(Op::srawi, {dst(), src(), range(0, 31), rc()}); return;
      case 8:
        a.op(chance(50) ? Op::rlwinm : Op::rlwimi, {dst(), src(), range(0, 31), range(0, 31), range(0, 31), rc()});
        return;
      default: a.op(Op::rlwnm, {dst(), src(), src(), range(0, 31), range(0, 31), rc()}); return;
    }
  }

  void gen_cr(Asm& a) {
    static const Op kCrBit[] = {Op::crand, Op::crandc, Op::creqv, Op::crnand,
                                Op::crnor, Op::cror, Op::crorc, Op::crxor};
    switch (range(0, 7)) {
      case 0: a.op(Op::cmpi, {range(0, 7), src(), simm()}); return;
      case 1: a.op(Op::cmpli, {range(0, 7), src(), range(0, 0xFFFF)}); return;
      case 2: a.op(chance(50) ? Op::cmp : Op::cmpl, {range(0, 7), src(), src()}); return;
      case 3: a.op(pick(kCrBit), {range(0, 31), range(0, 31), range(0, 31)}); return;
      case 4: a.op(Op::mcrf, {range(0, 7), range(0, 7)}); return;
      case 5: a.op(Op::mcrxr, {range(0, 7)}); return;
      case 6: a.op(Op::mfcr, {dst()}); return;
      default: a.op(Op::mtcrf, {range(0, 255), src()}); return;
    }
  }

  void gen_fp(Asm& a) {
    static const Op kAB[] = {Op::fadd, Op::fadds, Op::fsub, Op::fsubs, Op::fdiv, Op::fdivs,
                             Op::ps_add, Op::ps_sub, Op::ps_div};
    static const Op kAC[] = {Op::fmul, Op::fmuls, Op::ps_mul, Op::ps_muls0, Op::ps_muls1};
    static const Op kABC[] = {Op::fmadd, Op::fmadds, Op::fmsub, Op::fmsubs, Op::fnmadd,
                              Op::fnmadds, Op::fnmsub, Op::fnmsubs, Op::fsel, Op::ps_sel,
                              Op::ps_sum0, Op::ps_sum1, Op::ps_madds0, Op::ps_madds1,
                              Op::ps_madd, Op::ps_msub, Op::ps_nmadd, Op::ps_nmsub};
    static const Op kB[] = {Op::fres, Op::frsqrte, Op::ps_res, Op::ps_rsqrte};
    static const Op kXB[] = {Op::fmr, Op::fneg, Op::fabs_, Op::fnabs, Op::frsp, Op::fctiw,
                             Op::fctiwz, Op::ps_mr, Op::ps_neg, Op::ps_abs, Op::ps_nabs};
    static const Op kMerge[] = {Op::ps_merge00, Op::ps_merge01, Op::ps_merge10, Op::ps_merge11};
    static const Op kCmp[] = {Op::fcmpu, Op::fcmpo, Op::ps_cmpu0, Op::ps_cmpo0, Op::ps_cmpu1, Op::ps_cmpo1};
    switch (range(0, 11)) {
      case 0:
      case 1: a.op(pick(kAB), {fpr(), fpr(), fpr(), rc()}); return;
      case 2: a.op(pick(kAC), {fpr(), fpr(), fpr(), rc()}); return;
      case 3:
      case 4: a.op(pick(kABC), {fpr(), fpr(), fpr(), fpr(), rc()}); return;
      case 5: a.op(pick(kB), {fpr(), fpr(), rc()}); return;
      case 6:
      case 7: a.op(pick(kXB), {fpr(), fpr(), rc()}); return;
      case 8: a.op(pick(kMerge), {fpr(), fpr(), fpr(), rc()}); return;
      case 9: a.op(pick(kCmp), {range(0, 7), fpr(), fpr()}); return;
      case 10: a.op(Op::mffs, {fpr(), rc()}); return;
      default:
        switch (range(0, 4)) {
          case 0: a.op(Op::mtfsfi, {range(0, 7), range(0, 15), rc()}); return;
          case 1: a.op(Op::mtfsf, {range(0, 255), fpr(), rc()}); return;
          case 2: a.op(Op::mtfsb0, {range(0, 31), rc()}); return;
          case 3: a.op(Op::mtfsb1, {range(0, 31), rc()}); return;
          default: a.op(Op::mcrfs, {range(0, 7), range(0, 7)}); return;
        }
    }
  }

  // Memory operands always address the scratch area: r31 = scratch base,
  // r30 = scratch base (update forms), r29 = small index (indexed forms).
  void gen_memory(Asm& a) {
    auto disp = [&](u32 align) { return static_cast<s32>(range(0, 0x7F0 / align) * align); };
    auto set_index = [&](u32 align) { a.op(Op::addi, {29, 0, disp(align)}); };
    auto set_update_base = [&] { a.op(Op::addi, {30, 31, static_cast<s32>(range(0, 0x100) * 4)}); };
    static const Op kLoad[] = {Op::lwz, Op::lbz, Op::lhz, Op::lha};
    static const Op kLoadU[] = {Op::lwzu, Op::lbzu, Op::lhzu, Op::lhau};
    static const Op kStore[] = {Op::stw, Op::stb, Op::sth};
    static const Op kStoreU[] = {Op::stwu, Op::stbu, Op::sthu};
    static const Op kLoadX[] = {Op::lwzx, Op::lbzx, Op::lhzx, Op::lhax, Op::lhbrx, Op::lwbrx, Op::eciwx};
    static const Op kLoadUX[] = {Op::lwzux, Op::lbzux, Op::lhzux, Op::lhaux};
    static const Op kStoreX[] = {Op::stwx, Op::stbx, Op::sthx, Op::sthbrx, Op::stwbrx, Op::ecowx};
    static const Op kStoreUX[] = {Op::stwux, Op::stbux, Op::sthux};
    static const Op kFLoad[] = {Op::lfs, Op::lfd};
    static const Op kFLoadU[] = {Op::lfsu, Op::lfdu};
    static const Op kFStore[] = {Op::stfs, Op::stfd};
    static const Op kFStoreU[] = {Op::stfsu, Op::stfdu};
    static const Op kFLoadX[] = {Op::lfsx, Op::lfdx};
    static const Op kFStoreX[] = {Op::stfsx, Op::stfdx, Op::stfiwx};
    static const Op kFUX[] = {Op::lfsux, Op::lfdux, Op::stfsux, Op::stfdux};
    switch (range(0, 17)) {
      case 0: a.op(pick(kLoad), {dst(), 31, disp(4)}); return;
      case 1: set_update_base(); a.op(pick(kLoadU), {dst(), 30, disp(4)}); return;
      case 2: a.op(pick(kStore), {src(), 31, disp(4)}); return;
      case 3: set_update_base(); a.op(pick(kStoreU), {src(), 30, disp(4)}); return;
      case 4: set_index(4); a.op(pick(kLoadX), {dst(), 31, 29}); return;
      case 5: set_update_base(); set_index(4); a.op(pick(kLoadUX), {dst(), 30, 29}); return;
      case 6: set_index(4); a.op(pick(kStoreX), {src(), 31, 29}); return;
      case 7: set_update_base(); set_index(4); a.op(pick(kStoreUX), {src(), 30, 29}); return;
      case 8: a.op(pick(kFLoad), {fpr(), 31, disp(8)}); return;
      case 9: set_update_base(); a.op(pick(kFLoadU), {fpr(), 30, disp(8)}); return;
      case 10: a.op(pick(kFStore), {fpr(), 31, disp(8)}); return;
      case 11: set_update_base(); a.op(pick(kFStoreU), {fpr(), 30, disp(8)}); return;
      case 12: set_index(8); a.op(pick(kFLoadX), {fpr(), 31, 29}); return;
      case 13: set_index(8); a.op(pick(kFStoreX), {fpr(), 31, 29}); return;
      case 14: set_update_base(); set_index(8); a.op(pick(kFUX), {fpr(), 30, 29}); return;
      case 15: {
        static const Op kPsq[] = {Op::psq_l, Op::psq_st, Op::psq_lu, Op::psq_stu};
        const Op o = pick(kPsq);
        const bool upd = o == Op::psq_lu || o == Op::psq_stu;
        if (upd) set_update_base();
        a.op(o, {fpr(), upd ? 30u : 31u, static_cast<s32>(range(0, 0xF0) * 8), range(0, 1), range(0, 7)});
        return;
      }
      case 16: {
        static const Op kPsqX[] = {Op::psq_lx, Op::psq_stx, Op::psq_lux, Op::psq_stux};
        const Op o = pick(kPsqX);
        const bool upd = o == Op::psq_lux || o == Op::psq_stux;
        if (upd) set_update_base();
        set_index(8);
        a.op(o, {fpr(), upd ? 30u : 31u, 29, range(0, 1), range(0, 7)});
        return;
      }
      default:
        switch (range(0, 4)) {
          case 0: set_index(32); a.op(Op::dcbz, {31, 29}); return;
          case 1: a.op(Op::stmw, {range(20, 31), 31, disp(4)}); return;
          case 2: set_index(4); a.op(Op::lwarx, {dst(), 31, 29}); return;
          case 3: set_index(4); a.op(Op::stwcx_rc, {src(), 31, 29}); return;
          default: a.op(Op::dcbf, {31, 29}); return;
        }
    }
  }

  void gen_spr(Asm& a) {
    static const u32 kRead[] = {1, 8, 9, 912, 913, 914, 915, 916, 917, 918, 919, 920};
    static const u32 kWrite[] = {1, 9, 912, 913, 914, 915, 916, 917, 918, 919};
    if (chance(50)) a.op(Op::mfspr, {dst(), pick(kRead)});
    else a.op(Op::mtspr, {pick(kWrite), src()});
  }

  template <typename T, std::size_t N>
  T pick(const T (&arr)[N]) {
    return arr[range(0, N - 1)];
  }

  std::mt19937 rng_;
  std::map<std::string, int> pending_;
  int fn_counter_ = 0;
};

bool write_text(const std::filesystem::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
  return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: fixturegen <output-dir>\n");
    return 2;
  }
  const std::filesystem::path out(argv[1]);
  std::filesystem::create_directories(out);

  Fixture fx;
  hand_written(fx);

  RandomGen rng(0x0DEC0DE5);
  for (int i = 0; i < kRandomFunctions; ++i) {
    const std::string name = "fn_random_" + std::to_string(i);
    fx.begin(name);
    rng.gen_function(fx.a, static_cast<int>(rng.range(10, 60)));
    fx.end();
    fx.test(name, 1);
  }
  fx.a.resolve();

  // Data section: resolve label references.
  std::vector<u8> data;
  for (const auto& [ref, value] : fx.data_words) {
    u32 v = value;
    if (!ref.empty()) v = fx.a.address_of(ref);
    for (int s = 24; s >= 0; s -= 8) data.push_back(static_cast<u8>(v >> s));
  }
  // jt_config_ptr holds the address of jt_config.
  {
    const u32 off = fx.addr("jt_config_ptr") - kData;
    const u32 v = fx.addr("jt_config");
    for (int i = 0; i < 4; ++i) data[off + i] = static_cast<u8>(v >> (24 - 8 * i));
  }

  recomp::DolBuilder dol;
  dol.add_text(kText, fx.a.bytes());
  dol.add_data(kData, data);
  dol.set_entry(kText);
  const std::vector<u8> dol_bytes = dol.build();
  std::string error;
  if (!recomp::write_file((out / "fixture.dol").string(), dol_bytes.data(), dol_bytes.size(), &error)) {
    std::fprintf(stderr, "fixturegen: %s\n", error.c_str());
    return 1;
  }

  // symbols.txt in decomp-toolkit format.
  recomp::SymbolFile syms;
  for (const FunctionInfo& fn : fx.functions) {
    if (!fn.in_symbols) continue;
    recomp::Symbol s;
    s.name = fn.name;
    s.section = ".text";
    s.address = fn.start;
    s.type = recomp::Symbol::Type::Function;
    s.size = fn.end - fn.start;
    s.scope = "global";
    syms.symbols.push_back(s);
  }
  for (const auto& [name, offset] : fx.data_symbols) {
    recomp::Symbol s;
    s.name = name;
    s.section = ".data";
    s.address = kData + offset;
    s.type = recomp::Symbol::Type::Object;
    s.size = fx.data_sizes[name];
    s.scope = "local";
    syms.symbols.push_back(s);
  }
  write_text(out / "fixture_symbols.txt", syms.serialize());

  char buf[256];
  std::string toml =
      "# Generated by fixturegen.\n[input]\ndol = \"fixture.dol\"\nsymbols = \"fixture_symbols.txt\"\n\n"
      "[output]\ndirectory = \"generated\"\nfile_count = 4\n\n";
  std::snprintf(buf, sizeof(buf), "[analysis]\nsda_base = 0x%08X\nsda2_base = 0x%08X\n", kSdaBase, kSda2Base);
  toml += buf;
  toml += fx.config_extra;
  write_text(out / "fixture.toml", toml);

  std::string cases;
  for (const CaseInfo& c : fx.cases) {
    std::snprintf(buf, sizeof(buf), "FIXTURE_CASE(\"%s\", 0x%08Xu, %d, %u)\n", c.name.c_str(),
                  fx.a.address_of(c.name), c.kind, c.arg_mod);
    cases += buf;
  }
  write_text(out / "fixture_cases.inc", cases);
  std::snprintf(buf, sizeof(buf),
                "#define FIXTURE_SCRATCH 0x%08Xu\n#define FIXTURE_SDA_BASE 0x%08Xu\n#define FIXTURE_SDA2_BASE 0x%08Xu\n",
                kScratch, kSdaBase, kSda2Base);
  write_text(out / "fixture_layout.h", buf);

  std::printf("fixturegen: %zu functions, %zu test cases -> %s\n", fx.functions.size(), fx.cases.size(),
              out.string().c_str());
  return 0;
}
