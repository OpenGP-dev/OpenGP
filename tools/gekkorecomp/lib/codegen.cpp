// SPDX-License-Identifier: GPL-3.0-or-later

#include "codegen.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace recomp {

using gekko::Op;
namespace f = gekko::fields;

namespace {

std::string hex32(u32 v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
  return buf;
}

std::string label(u32 addr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "loc_%08X", addr);
  return buf;
}

std::string format_arg(const gekko::FieldDesc& fd, s64 v) {
  char buf[32];
  switch (fd.kind) {
    case gekko::FieldKind::Hex:
    case gekko::FieldKind::UImm:
      std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
      return buf;
    default:
      return std::to_string(v);
  }
}

std::string args_of(u32 raw, gekko::Sig sig) {
  std::string out;
  for (const gekko::FieldDesc& fd : gekko::sig_fields(sig)) out += ", " + format_arg(fd, gekko::field_get(fd, raw));
  return out;
}

class FunctionEmitter {
 public:
  FunctionEmitter(const Program& p, const FunctionAnalysis& a) : p_(p), a_(a), fn_(*a.function) {}

  std::string run() {
    for (u32 pc = fn_.start; pc < fn_.end; pc += 4) emit_instruction(pc);
    emit_fallthrough();

    std::string out;
    char range[96];
    std::snprintf(range, sizeof(range), "// %08X - %08X", fn_.start, fn_.end);
    out += range;
    if (!fn_.symbol.empty() && fn_.symbol != fn_.cpp_name) out += " (" + fn_.symbol + ")";
    out += "\nPPC_FUNC_IMPL(__imp__" + fn_.cpp_name + ") {\n";
    out += "  PPC_TRACE_ENTER(" + hex32(fn_.start) + ");\n";
    if (needs_entry_capture_)
      out += "  const ::ppc::u32 entry_lr = ctx.lr;\n  const ::ppc::u32 entry_sp = ctx.gpr[1];\n";
    out += body_.str();
    out += "}\nPPC_WEAK_FUNC(" + fn_.cpp_name + ") { __imp__" + fn_.cpp_name + "(ctx, base); }\n\n";
    return out;
  }

 private:
  bool inside(u32 addr) const { return addr >= fn_.start && addr < fn_.end; }

  void line(const std::string& s) { body_ << "  " << s << "\n"; }

  std::string jump_out(const std::string& target_expr) {
    needs_entry_capture_ = true;
    return "::ppc::jump(ctx, base, " + target_expr + ", entry_lr, entry_sp); return;";
  }

  // Code for "transfer control to `target` and do not come back" (b / bc / fallthrough).
  std::string goto_or_tail(u32 pc, u32 target) {
    if (inside(target)) {
      std::string s;
      if (p_.config().interrupt_checks && target <= pc) s = "::ppc::check_interrupts(ctx, base); ";
      return s + "goto " + label(target) + ";";
    }
    if (const Function* callee = p_.function_at(target)) return callee->cpp_name + "(ctx, base); return;";
    return jump_out(hex32(target));
  }

  // Code for a call to `target` (bl / bcl), after LR has been set.
  std::string call(u32 target) {
    if (const Function* callee = p_.function_at(target)) {
      if (p_.config().exception_return.count(callee->symbol))
        return callee->cpp_name + "(ctx, base); if (ctx.exception_return) return;";
      return callee->cpp_name + "(ctx, base);";
    }
    return "::ppc::call(ctx, base, " + hex32(target) + ");";
  }

  void emit_instruction(u32 pc) {
    const u32 raw = a_.word(pc);
    const Op op = a_.op(pc);
    if (a_.labels.count(pc)) body_ << label(pc) << ":\n";
    char comment[128];
    std::snprintf(comment, sizeof(comment), "// %08X: %s", pc, gekko::disasm(raw, pc).c_str());
    line(comment);

    bool skip = false;
    if (const auto it = p_.config().hooks.find(pc); it != p_.config().hooks.end()) {
      line(it->second.function + "(ctx, base);");
      skip = it->second.skip;
      if (it->second.return_after) {
        line("return;");
        return;
      }
    }
    if (skip) return;

    const gekko::OpInfo& info = gekko::op_info(op);
    if (op == Op::Invalid) {
      line("::ppc::fatal(\"invalid instruction " + hex32(raw) + " at " + hex32(pc) + "\");");
      return;
    }
    switch (info.cc) {
      case gekko::CallConv::CTX:
        line(std::string("::ppc::op::") + info.id + "(ctx" + args_of(raw, info.sig) + ");");
        return;
      case gekko::CallConv::BASE:
        line(std::string("::ppc::op::") + info.id + "(ctx, base" + args_of(raw, info.sig) + ");");
        return;
      case gekko::CallConv::PC:
        line(std::string("::ppc::op::") + info.id + "(ctx, base, " + hex32(pc) + args_of(raw, info.sig) + ");");
        return;
      case gekko::CallConv::MEM: {
        const auto it = a_.accesses.find(pc);
        const AccessKind kind = it == a_.accesses.end() ? AccessKind::Checked : it->second;
        line(std::string("::ppc::op::") + info.id + "<::ppc::Mem::" + access_kind_name(kind) +
             ">(ctx, base" + args_of(raw, info.sig) + ");");
        return;
      }
      case gekko::CallConv::BRANCH:
        emit_branch(pc, raw, op);
        return;
    }
  }

  void emit_branch(u32 pc, u32 raw, Op op) {
    const u32 lk = f::lk::get(raw);
    const u32 next = pc + 4;
    switch (op) {
      case Op::b: {
        const u32 target = (f::aa::get(raw) ? 0 : pc) + static_cast<u32>(f::li::get(raw));
        if (!lk) {
          line(goto_or_tail(pc, target));
        } else {
          line("ctx.lr = " + hex32(next) + ";");
          if (target != next) line(call(target));  // "bl $+4" only reads the PC
        }
        return;
      }
      case Op::bc: {
        const u32 bo = f::bo::get(raw), bi = f::bi::get(raw);
        const u32 target = (f::aa::get(raw) ? 0 : pc) + static_cast<u32>(f::bd::get(raw));
        const bool always = (bo & 0x14) == 0x14;
        const std::string cond = "::ppc::bc_cond(ctx, " + std::to_string(bo) + ", " + std::to_string(bi) + ")";
        if (lk) {
          // LR is written whether or not the branch is taken.
          line("{ const bool taken = " + (always ? std::string("true") : cond) + "; ctx.lr = " + hex32(next) +
               "; if (taken) { " + call(target) + " } }");
        } else if (always) {
          line(goto_or_tail(pc, target));
        } else {
          line("if (" + cond + ") { " + goto_or_tail(pc, target) + " }");
        }
        return;
      }
      case Op::bclr: {
        const u32 bo = f::bo::get(raw), bi = f::bi::get(raw);
        const bool always = (bo & 0x14) == 0x14;
        const std::string cond = "::ppc::bc_cond(ctx, " + std::to_string(bo) + ", " + std::to_string(bi) + ")";
        if (lk) {
          line("{ const bool taken = " + (always ? std::string("true") : cond) +
               "; const ::ppc::u32 target = ctx.lr & ~3u; ctx.lr = " + hex32(next) +
               "; if (taken) ::ppc::call(ctx, base, target); }");
        } else if (always) {
          line("return;");
        } else {
          line("if (" + cond + ") return;");
        }
        return;
      }
      case Op::bcctr: {
        const u32 bo = f::bo::get(raw), bi = f::bi::get(raw);
        const bool always = (bo & 0x14) == 0x14;
        const std::string cond = "::ppc::bcctr_cond(ctx, " + std::to_string(bo) + ", " + std::to_string(bi) + ")";
        if (lk) {
          line("{ const bool taken = " + (always ? std::string("true") : cond) +
               "; const ::ppc::u32 target = ctx.ctr & ~3u; ctx.lr = " + hex32(next) +
               "; if (taken) ::ppc::call(ctx, base, target); }");
          return;
        }
        std::string transfer;
        if (const auto jt = a_.jump_tables.find(pc); jt != a_.jump_tables.end()) {
          std::ostringstream sw;
          sw << "switch (ctx.ctr & ~3u) {";
          std::set<u32> seen;
          for (u32 t : jt->second.targets) {
            if (!seen.insert(t).second) continue;
            sw << " case " << hex32(t) << ": " << goto_or_tail(pc, t);
          }
          sw << " default: " << jump_out("ctx.ctr & ~3u") << " }";
          transfer = sw.str();
        } else {
          transfer = jump_out("ctx.ctr & ~3u");
        }
        if (always) line(transfer);
        else line("if (" + cond + ") { " + transfer + " }");
        return;
      }
      case Op::sc:
        line("::ppc::g_hooks.syscall(ctx, base, " + hex32(pc) + ");");
        return;
      case Op::rfi:
        line("::ppc::g_hooks.rfi(ctx, base, " + hex32(pc) + "); return;");
        return;
      default:
        line("::ppc::fatal(\"unhandled branch at " + hex32(pc) + "\");");
        return;
    }
  }

  // If the last instruction can fall through, continue into the next function.
  void emit_fallthrough() {
    const u32 last = fn_.end - 4;
    const u32 raw = a_.word(last);
    const Op op = a_.op(last);
    const u32 lk = f::lk::get(raw);
    bool ends = false;
    switch (op) {
      case Op::b: ends = !lk; break;
      case Op::bc:
      case Op::bclr:
      case Op::bcctr: ends = !lk && (f::bo::get(raw) & 0x14) == 0x14; break;
      case Op::rfi: ends = true; break;
      default: break;
    }
    if (const auto it = p_.config().hooks.find(last); it != p_.config().hooks.end() && it->second.return_after)
      ends = true;
    if (ends) return;
    line("// falls through to " + hex32(fn_.end));
    if (const Function* next = p_.function_at(fn_.end)) line(next->cpp_name + "(ctx, base); return;");
    else line(jump_out(hex32(fn_.end)));
  }

  const Program& p_;
  const FunctionAnalysis& a_;
  const Function& fn_;
  std::ostringstream body_;
  bool needs_entry_capture_ = false;
};

bool write_text(const std::filesystem::path& path, const std::string& text, std::string* error) {
  // Only rewrite files whose contents changed, so incremental builds stay fast.
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::stringstream ss;
      ss << in.rdbuf();
      if (ss.str() == text) return true;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "cannot write " + path.string();
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

}  // namespace

std::string emit_function(const Program& program, const FunctionAnalysis& analysis) {
  return FunctionEmitter(program, analysis).run();
}

bool generate(const Program& program, const std::string& directory, GenerateStats* stats,
              std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec) {
    if (error) *error = "cannot create " + directory + ": " + ec.message();
    return false;
  }
  const fs::path dir(directory);
  const Config& config = program.config();
  const auto& functions = program.functions();
  GenerateStats st;
  std::ostringstream report;

  // Shared header.
  {
    std::ostringstream h;
    h << "// Generated by gekkorecomp. Derived from the game: do not commit.\n#pragma once\n\n"
      << "#include \"ppc/recomp.h\"\n#include \"ppc/module.h\"\n\nnamespace guest {\n\n";
    for (const Function& fn : functions)
      h << "PPC_EXTERN_FUNC(" << fn.cpp_name << ");\nPPC_EXTERN_FUNC(__imp__" << fn.cpp_name << ");\n";
    std::set<std::string> hooks;
    for (const auto& [addr, hook] : config.hooks) hooks.insert(hook.function);
    if (!hooks.empty()) h << "\n// Mid-function hooks (defined by the game project).\n";
    for (const std::string& name : hooks) h << "PPC_HOOK(" << name << ");\n";
    h << "\nextern const ::ppc::RecompiledModule g_module;\n\n}  // namespace guest\n";
    if (!write_text(dir / "ppc_recomp_shared.h", h.str(), error)) return false;
  }

  // Function bodies, spread evenly over file_count files.
  const std::size_t per_file = (functions.size() + config.file_count - 1) / config.file_count;
  std::size_t index = 0;
  for (u32 file = 0; file < config.file_count; ++file) {
    std::ostringstream src;
    src << "// Generated by gekkorecomp. Derived from the game: do not commit.\n"
        << "#include \"ppc_recomp_shared.h\"\n\nnamespace guest {\n\n";
    for (std::size_t i = 0; i < per_file && index < functions.size(); ++i, ++index) {
      const Function& fn = functions[index];
      const FunctionAnalysis a = program.analyze(fn);
      src << emit_function(program, a);
      ++st.functions;
      st.instructions += a.code.size();
      st.jump_tables += a.jump_tables.size();
      for (const auto& [pc, kind] : a.accesses) {
        if (kind == AccessKind::Checked) ++st.checked_accesses;
        if (kind == AccessKind::Mmio || kind == AccessKind::GatherPipe) ++st.io_accesses;
      }
      for (const std::string& w : a.warnings) {
        report << fn.cpp_name << ": " << w << "\n";
        ++st.warnings;
      }
    }
    src << "}  // namespace guest\n";
    if (!write_text(dir / ("ppc_recomp." + std::to_string(file) + ".cpp"), src.str(), error)) return false;
  }

  // Function table, patches and module description.
  {
    std::ostringstream m;
    m << "// Generated by gekkorecomp. Derived from the game: do not commit.\n"
      << "#include \"ppc_recomp_shared.h\"\n\nnamespace guest {\n\n"
      << "static const ::ppc::FuncMapping kFunctions[] = {\n";
    for (const Function& fn : functions) m << "    {" << hex32(fn.start) << ", " << fn.cpp_name << "},\n";
    m << "};\n\n";
    if (!config.patches.empty()) {
      m << "static const ::ppc::InstructionPatch kPatches[] = {\n";
      for (const auto& [addr, value] : config.patches) m << "    {" << hex32(addr) << ", " << hex32(value) << "},\n";
      m << "};\n\n";
    }
    m << "extern const ::ppc::RecompiledModule g_module = {\n"
      << "    kFunctions,\n    sizeof(kFunctions) / sizeof(kFunctions[0]),\n"
      << (config.patches.empty() ? "    nullptr,\n    0,\n"
                                 : "    kPatches,\n    sizeof(kPatches) / sizeof(kPatches[0]),\n")
      << "    " << hex32(program.dol().entry_point()) << ",\n"
      << "    " << hex32(config.sda_base.value_or(0)) << ",\n"
      << "    " << hex32(config.sda2_base.value_or(0)) << ",\n};\n\n}  // namespace guest\n";
    if (!write_text(dir / "ppc_func_mapping.cpp", m.str(), error)) return false;
  }

  // Report.
  {
    std::ostringstream r;
    r << "gekkorecomp report\n"
      << "functions:          " << st.functions << "\n"
      << "instructions:       " << st.instructions << "\n"
      << "jump tables:        " << st.jump_tables << "\n"
      << "I/O accesses:       " << st.io_accesses << "\n"
      << "checked accesses:   " << st.checked_accesses << "\n"
      << "warnings:           " << st.warnings + program.warnings().size() << "\n\n";
    for (const std::string& w : program.warnings()) r << w << "\n";
    r << report.str();
    if (!write_text(dir / "recomp_report.txt", r.str(), error)) return false;
  }

  if (stats) *stats = st;
  return true;
}

}  // namespace recomp
