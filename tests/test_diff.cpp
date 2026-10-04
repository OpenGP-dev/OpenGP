// SPDX-License-Identifier: GPL-3.0-or-later
//
// Differential test: every function in the generated fixture DOL is run from
// the same random initial state through (a) the interpreter, with native
// calls disabled so it is an independent oracle, and (b) the recompiled
// code. Registers, scratch memory, the gather-pipe stream and MMIO writes
// must match exactly.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "fixture_layout.h"
#include "ppc/interpreter.h"
#include "ppc/memory.h"
#include "ppc/module.h"
#include "ppc_recomp_shared.h"

using namespace ppc;

namespace {

struct Case {
  const char* name;
  u32 address;
  int kind;  // 0 hand-written, 1 random, 2 recompiled only
  u32 arg_mod;
};

const Case kCases[] = {
#define FIXTURE_CASE(name, addr, kind, mod) {name, addr, kind, mod},
#include "fixture_cases.inc"
#undef FIXTURE_CASE
};

constexpr u32 kStackBase = 0x80370000;
constexpr u32 kStackSize = 0x10000;
constexpr u32 kStackTop = kStackBase + kStackSize - 0x100;
constexpr u32 kScratchSize = 0x10000;
constexpr u32 kReturnSentinel = 0x80000100;

struct MmioWrite {
  u32 addr, value, size;
  bool operator==(const MmioWrite&) const = default;
};
std::vector<MmioWrite> g_mmio_writes;

u32 test_mmio_read(u32 addr, u32 size) { return (addr * 2654435761u) >> (32 - 8 * size); }
void test_mmio_write(u32 addr, u32 value, u32 size) { g_mmio_writes.push_back({addr, value, size}); }

struct Env {
  GuestMemory mem;
  u8* base = nullptr;
  Env() {
    std::string err;
    REQUIRE_MESSAGE(mem.init(MEM1_SIZE_GAMECUBE, &err), err);
    base = mem.base();
    std::vector<u8> dol;
    std::ifstream f(FIXTURE_DOL, std::ios::binary);
    REQUIRE(f);
    dol.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    u32 entry = 0;
    REQUIRE_MESSAGE(load_dol(dol.data(), dol.size(), base, &entry, &err), err);
    install_module(guest::g_module, base);
  }
};

Env& env() {
  static Env e;
  return e;
}

u64 random_fpr_bits(std::mt19937& rng) {
  const u32 pick = rng() % 10;
  auto rnd64 = [&] { return (u64{rng()} << 32) | rng(); };
  switch (pick) {
    case 0:
    case 1: return rnd64();  // anything, including NaNs and denormals
    case 2:
    case 3:
    case 4: {                // a single-precision value
      float f;
      const u32 b = rng();
      std::memcpy(&f, &b, 4);
      return std::bit_cast<u64>(static_cast<double>(f));
    }
    case 5:
    case 6: return std::bit_cast<u64>(static_cast<double>(static_cast<int>(rng() % 2001) - 1000) / 8.0);
    default: {
      static const u64 kSpecial[] = {
          0x0000000000000000ULL, 0x8000000000000000ULL,  // +/-0
          0x7FF0000000000000ULL, 0xFFF0000000000000ULL,  // +/-inf
          0x7FF8000000000000ULL, 0x7FF80000DEADBEEFULL,  // quiet NaNs
          0x7FF00000000BEEF0ULL, 0xFFF0000000000001ULL,  // signalling NaNs
          0x0000000000000001ULL, 0x800FFFFFFFFFFFFFULL,  // double denormals
          0x3690000000000000ULL, 0x3810000000000000ULL,  // single denormal / smallest normal
          0x47EFFFFFE0000000ULL, 0x3FF0000000000000ULL,  // FLT_MAX, 1.0
      };
      return kSpecial[rng() % std::size(kSpecial)];
    }
  }
}

void init_state(Context& c, u8* base, const Case& k, u32 seed) {
  std::mt19937 rng(seed);
  reset_context(c);
  for (u32& r : c.gpr) r = rng();
  c.gpr[1] = kStackTop;
  c.gpr[2] = FIXTURE_SDA2_BASE;
  c.gpr[13] = FIXTURE_SDA_BASE;
  c.gpr[29] = 0;
  c.gpr[30] = FIXTURE_SCRATCH;
  c.gpr[31] = FIXTURE_SCRATCH;
  if (k.arg_mod) c.gpr[3] %= k.arg_mod;
  for (PairedSingle& f : c.fpr) {
    f.ps0 = random_fpr_bits(rng);
    f.ps1 = random_fpr_bits(rng);
  }
  c.cr = rng();
  const u32 xer = rng();
  c.xer_so = xer >> 31;
  c.xer_ov = (xer >> 30) & 1;
  c.xer_ca = (xer >> 29) & 1;
  c.xer_bc = static_cast<u16>(rng() % 24);
  c.ctr = 1 + rng() % 40;
  c.fpscr = rng() & 0x7;  // rounding mode and NI
  c.lr = kReturnSentinel;
  for (u32 i = 0; i < 8; ++i) {
    static const u32 kTypes[] = {0, 4, 5, 6, 7, 0, 4, 7, 1};
    const u32 ld = kTypes[rng() % std::size(kTypes)], st = kTypes[rng() % std::size(kTypes)];
    c.spr[spr::GQR0 + i] = ((rng() % 64) << 24) | (ld << 16) | ((rng() % 64) << 8) | st;
  }
  for (u32 i = 0; i < kScratchSize; i += 4) store_be<u32>(base + FIXTURE_SCRATCH + i, rng());
  std::memset(base + kStackBase, 0, kStackSize);
}

struct Snapshot {
  Context ctx;
  std::vector<u8> scratch, stack;
  std::vector<u8> gather_pipe;
  std::vector<MmioWrite> mmio;
};

Snapshot capture(const Context& c, u8* base) {
  Snapshot s;
  s.ctx = c;
  s.scratch.assign(base + FIXTURE_SCRATCH, base + FIXTURE_SCRATCH + kScratchSize);
  s.stack.assign(base + kStackBase, base + kStackBase + kStackSize);
  s.gather_pipe = g_default_gather_pipe;
  s.mmio = g_mmio_writes;
  return s;
}

std::string hex(u64 v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(v));
  return buf;
}

// Returns a description of the first difference, or "" if identical.
std::string compare(const Snapshot& a, const Snapshot& b) {
  for (int r = 0; r < 32; ++r)
    if (a.ctx.gpr[r] != b.ctx.gpr[r])
      return "r" + std::to_string(r) + ": interp " + hex(a.ctx.gpr[r]) + " recomp " + hex(b.ctx.gpr[r]);
  for (int r = 0; r < 32; ++r) {
    if (a.ctx.fpr[r].ps0 != b.ctx.fpr[r].ps0)
      return "f" + std::to_string(r) + ".ps0: interp " + hex(a.ctx.fpr[r].ps0) + " recomp " + hex(b.ctx.fpr[r].ps0);
    if (a.ctx.fpr[r].ps1 != b.ctx.fpr[r].ps1)
      return "f" + std::to_string(r) + ".ps1: interp " + hex(a.ctx.fpr[r].ps1) + " recomp " + hex(b.ctx.fpr[r].ps1);
  }
  if (a.ctx.cr != b.ctx.cr) return "cr: interp " + hex(a.ctx.cr) + " recomp " + hex(b.ctx.cr);
  if (a.ctx.lr != b.ctx.lr) return "lr: interp " + hex(a.ctx.lr) + " recomp " + hex(b.ctx.lr);
  if (a.ctx.ctr != b.ctx.ctr) return "ctr: interp " + hex(a.ctx.ctr) + " recomp " + hex(b.ctx.ctr);
  if (get_xer(a.ctx) != get_xer(b.ctx)) return "xer: interp " + hex(get_xer(a.ctx)) + " recomp " + hex(get_xer(b.ctx));
  if (a.ctx.fpscr != b.ctx.fpscr) return "fpscr: interp " + hex(a.ctx.fpscr) + " recomp " + hex(b.ctx.fpscr);
  if (a.ctx.msr != b.ctx.msr) return "msr differs";
  if (a.ctx.reserve != b.ctx.reserve || (a.ctx.reserve && a.ctx.reserve_addr != b.ctx.reserve_addr))
    return "reservation differs";
  if (std::memcmp(a.ctx.spr, b.ctx.spr, sizeof(a.ctx.spr)) != 0) return "SPRs differ";
  for (std::size_t i = 0; i < a.scratch.size(); ++i)
    if (a.scratch[i] != b.scratch[i])
      return "scratch memory at +" + hex(i) + ": interp " + hex(a.scratch[i]) + " recomp " + hex(b.scratch[i]);
  for (std::size_t i = 0; i < a.stack.size(); ++i)
    if (a.stack[i] != b.stack[i]) return "stack memory at +" + hex(i);
  if (a.gather_pipe != b.gather_pipe)
    return "gather pipe: interp " + std::to_string(a.gather_pipe.size()) + " bytes, recomp " +
           std::to_string(b.gather_pipe.size()) + " bytes";
  if (!(a.mmio == b.mmio)) return "MMIO writes differ";
  return {};
}

void reset_io() {
  reset_hooks();
  g_hooks.mmio_read = test_mmio_read;
  g_hooks.mmio_write = test_mmio_write;
  g_default_gather_pipe.clear();
  g_mmio_writes.clear();
}

Snapshot run_interpreter(const Case& k, u32 seed) {
  Env& e = env();
  Context c;
  init_state(c, e.base, k, seed);
  reset_io();
  g_interpreter_options.call_native = false;
  interpret(c, e.base, k.address);
  g_interpreter_options.call_native = true;
  return capture(c, e.base);
}

Snapshot run_recompiled(const Case& k, u32 seed) {
  Env& e = env();
  Context c;
  init_state(c, e.base, k, seed);
  reset_io();
  GuestFunc fn = g_functions.find(k.address);
  REQUIRE_MESSAGE(fn != nullptr, "no recompiled function for ", k.name);
  fn(c, e.base);
  return capture(c, e.base);
}

}  // namespace

// Mid-function hook used by fn_hooked (see fixture.toml).
namespace guest {
PPC_HOOK(TestHook) {
  (void)base;
  ctx.gpr[6] = 0x1234;
}
}  // namespace guest

TEST_CASE("recompiled code matches the interpreter") {
  int compared = 0;
  for (const Case& k : kCases) {
    if (k.kind == 2) continue;
    const int iterations = k.kind == 0 ? 24 : 6;
    for (int it = 0; it < iterations; ++it) {
      const u32 seed = k.address * 31u + static_cast<u32>(it) * 7919u;
      const Snapshot a = run_interpreter(k, seed);
      const Snapshot b = run_recompiled(k, seed);
      const std::string diff = compare(a, b);
      INFO(k.name, " iteration ", it, ": ", diff);
      CHECK(diff.empty());
      ++compared;
    }
  }
  MESSAGE("compared ", compared, " runs over ", std::size(kCases), " functions");
}

TEST_CASE("hand-written functions compute the expected results") {
  auto find = [](const char* name) -> const Case& {
    for (const Case& k : kCases)
      if (std::strcmp(k.name, name) == 0) return k;
    FAIL("missing case ", name);
    return kCases[0];
  };
  auto run = [&](const char* name, u32 r3, u32 r4 = 0) {
    Env& e = env();
    Context c;
    init_state(c, e.base, find(name), 1);
    reset_io();
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    g_functions.find(find(name).address)(c, e.base);
    return c;
  };
  CHECK(run("fn_call_chain", 10).gpr[3] == 25u);  // (10 + 5) + 10
  for (u32 i = 0; i < 7; ++i) CHECK(run("fn_switch", i).gpr[4] == (i < 5 ? 10 * (i + 1) : 0xFFFFFFFFu));
  CHECK(run("fn_switch_heuristic", 1).gpr[3] == 201u);
  CHECK(run("fn_switch_heuristic", 4).gpr[3] == 0u);
  CHECK(run("fn_switch_config", 2).gpr[3] == (2u ^ 0x3333u));
  CHECK(run("fn_indirect_call", 6).gpr[3] == 9u);   // even: add 3
  CHECK(run("fn_indirect_call", 7).gpr[3] == 21u);  // odd: multiply by 3
  CHECK(run("fn_fib", 10).gpr[3] == 55u);
  CHECK(run("fn_tail_call", 1, 2).gpr[3] == 103u);
  CHECK(run("fn_ft_a", 0).gpr[3] == 3u);
  CHECK(run("fn_jump_middle", 5).gpr[3] == 16u);
  CHECK(run("fn_ptr_tail", 4).gpr[3] == 15u);
  CHECK(run("fn_patched", 0).gpr[3] == 2u);
  CHECK(run("fn_config_only", 1).gpr[3] == 8u);
  CHECK(run("fn_lmw_stmw", 1).gpr[31] == FIXTURE_SCRATCH);
  CHECK(run("fn_lmw_stmw", 1).gpr[3] == 1u + 27u + 1u + 31u);
  CHECK(run("fn_bl_next", 0).gpr[3] == find("fn_bl_next").address + 8);

  const Context hooked = run("fn_hooked", 0);
  CHECK(hooked.gpr[5] == 0u);
  CHECK(hooked.gpr[6] == 0x1234u);  // the hook replaced "li r6, 0"
}

TEST_CASE("gather pipe and MMIO accesses reach the hooks") {
  Env& e = env();
  const Case* gather = nullptr;
  for (const Case& k : kCases)
    if (std::strcmp(k.name, "fn_gather") == 0) gather = &k;
  REQUIRE(gather);
  Context c;
  init_state(c, e.base, *gather, 5);
  reset_io();
  g_functions.find(gather->address)(c, e.base);
  // 3 iterations x (stw 4 + stfs 4 + psq_st via r7: 1..8 + stb 1) bytes.
  CHECK(g_default_gather_pipe.size() >= 3 * (4 + 4 + 1 + 1));
  CHECK_FALSE(g_mmio_writes.empty());  // the psq_st at 0xCC00F800 is an MMIO write
}

TEST_CASE("generated code uses the expected access kinds and jump tables") {
  std::stringstream all;
  for (int i = 0; i < 4; ++i) {
    std::ifstream f(std::string(FIXTURE_GENERATED) + "/ppc_recomp." + std::to_string(i) + ".cpp");
    REQUIRE(f);
    all << f.rdbuf();
  }
  const std::string src = all.str();
  auto count = [&](const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = src.find(needle); pos != std::string::npos; pos = src.find(needle, pos + 1)) ++n;
    return n;
  };
  CHECK(count("switch (ctx.ctr") == 3);         // symbol-sized, scanned and configured tables
  CHECK(count("<::ppc::Mem::GatherPipe>") >= 3);  // fn_gather's stores through the lis constant
  CHECK(count("<::ppc::Mem::Mmio>") >= 4);        // fn_mmio + fn_gather's psq_st
  CHECK(count("TestHook(ctx, base);") == 1);
  CHECK(count("::ppc::jump(ctx, base,") >= 2);   // fn_jump_middle, fn_ptr_tail
}
