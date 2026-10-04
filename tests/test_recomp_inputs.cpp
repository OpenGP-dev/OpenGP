// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for gekkorecomp's inputs: decomp-toolkit symbol files, the TOML
// config, DOL parsing and identifier sanitising.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "analysis.h"
#include "config.h"
#include "dol.h"
#include "symbols.h"

using namespace recomp;

TEST_CASE("symbols.txt parsing") {
  const std::string text =
      "// comment line\n"
      "__start = .init:0x80003100; // type:function size:0x14C scope:global\n"
      "fn_80005678 = .text:0x80005678; // type:function size:0x40\n"
      "jumptable_80345678 = .data:0x80345678; // type:object size:0x28 scope:local\n"
      "lbl_80001234 = .text:0x80001234; // type:label scope:local\n"
      "\"name with space\" = .text:0x80001000; // type:function size:0x8 data:4byte\n"
      "_SDA_BASE_ = 0x80400000; // type:label\n"
      "\n";
  SymbolFile f;
  std::string err;
  REQUIRE_MESSAGE(f.parse(text, &err), err);
  REQUIRE(f.symbols.size() == 6);
  CHECK(f.symbols[0].name == "__start");
  CHECK(f.symbols[0].section == ".init");
  CHECK(f.symbols[0].address == 0x80003100u);
  CHECK(f.symbols[0].type == Symbol::Type::Function);
  CHECK(*f.symbols[0].size == 0x14Cu);
  CHECK(f.symbols[0].scope == "global");
  CHECK_FALSE(f.symbols[3].size.has_value());
  CHECK(f.symbols[3].type == Symbol::Type::Label);
  CHECK(f.symbols[4].name == "name with space");
  CHECK(f.symbols[4].attributes.at("data") == "4byte");
  CHECK(f.symbols[5].section.empty());
  CHECK(f.symbols[5].address == 0x80400000u);

  // Round trip.
  SymbolFile g;
  REQUIRE(g.parse(f.serialize(), &err));
  REQUIRE(g.symbols.size() == f.symbols.size());
  for (std::size_t i = 0; i < f.symbols.size(); ++i) {
    CHECK(g.symbols[i].name == f.symbols[i].name);
    CHECK(g.symbols[i].address == f.symbols[i].address);
    CHECK(g.symbols[i].size == f.symbols[i].size);
    CHECK(g.symbols[i].type == f.symbols[i].type);
  }

  SymbolFile bad;
  CHECK_FALSE(bad.parse("oops .text:0x80000000;\n", &err));
  CHECK_FALSE(bad.parse("x = .text:0xZZ;\n", &err));
}

TEST_CASE("recomp.toml parsing") {
  const auto dir = std::filesystem::temp_directory_path() / "opengp_test_config";
  std::filesystem::create_directories(dir);
  const auto path = dir / "recomp.toml";
  const std::string abs_symbols = (dir / "elsewhere" / "symbols.txt").string();
  {
    std::ofstream f(path);
    // TOML literal string ('...'): Windows paths contain backslashes.
    f << "[input]\ndol = \"main.dol\"\nsymbols = '" << abs_symbols << "'\n"
         "[output]\ndirectory = \"gen\"\nfile_count = 8\n"
         "[analysis]\nsda_base = 0x80400000\ninterrupt_checks = false\n"
         "[[function]]\naddress = 0x80003100\nsize = 0x40\nname = \"foo\"\n"
         "[[jump_table]]\nbctr = 0x80003120\ntable = 0x80300000\ncount = 5\n"
         "[[patch]]\naddress = 0x80003104\nvalue = 0x38600001\n"
         "[[hook]]\naddress = 0x80003108\nfunction = \"MyHook\"\nskip = true\nreturn = true\n";
  }
  Config c;
  std::string err;
  REQUIRE_MESSAGE(c.load(path.string(), &err), err);
  CHECK(c.dol_path == (dir / "main.dol").lexically_normal().string());
  CHECK(c.symbols_path == abs_symbols);
  CHECK(c.output_directory == "gen");
  CHECK(c.file_count == 8u);
  CHECK(c.sda_base == 0x80400000u);
  CHECK_FALSE(c.sda2_base.has_value());
  CHECK_FALSE(c.interrupt_checks);
  REQUIRE(c.functions.size() == 1);
  CHECK(c.functions[0].name == "foo");
  REQUIRE(c.jump_tables.size() == 1);
  CHECK(c.jump_tables[0].count == 5u);
  CHECK(c.patches.at(0x80003104) == 0x38600001u);
  CHECK(c.hooks.at(0x80003108).function == "MyHook");
  CHECK(c.hooks.at(0x80003108).skip);
  CHECK(c.hooks.at(0x80003108).return_after);

  {
    std::ofstream f(path);
    f << "[[patch]]\naddress = 0x80003104\n";  // missing value
  }
  Config bad;
  CHECK_FALSE(bad.load(path.string(), &err));
  CHECK(err.find("[[patch]]") != std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST_CASE("DOL round trip") {
  DolBuilder b;
  b.add_text(0x80003100, {0x4E, 0x80, 0x00, 0x20});
  b.add_data(0x80200000, {1, 2, 3, 4, 5, 6, 7, 8});
  b.set_bss(0x80300000, 0x100);
  b.set_entry(0x80003100);
  Dol d;
  std::string err;
  REQUIRE_MESSAGE(d.parse(b.build(), &err), err);
  REQUIRE(d.sections().size() == 2);
  CHECK(d.entry_point() == 0x80003100u);
  CHECK(d.bss_size() == 0x100u);
  CHECK(d.is_text(0x80003100));
  CHECK_FALSE(d.is_text(0x80200000));
  u32 w = 0;
  CHECK(d.read_u32(0x80003100, &w));
  CHECK(w == 0x4E800020u);
  CHECK(d.read_u32(0x80200004, &w));
  CHECK(w == 0x05060708u);
  CHECK_FALSE(d.read_u32(0x80200006, &w));  // runs past the section

  Dol tiny;
  CHECK_FALSE(tiny.parse(std::vector<u8>(16), &err));
}

TEST_CASE("identifier sanitising") {
  CHECK(sanitize_identifier("OSReport", 0x80001000) == "OSReport");
  CHECK(sanitize_identifier("", 0x80001000) == "fn_80001000");
  CHECK(sanitize_identifier("@123", 0x80001000) == "_123");
  CHECK(sanitize_identifier("9lives", 0x80001000) == "_9lives");
  CHECK(sanitize_identifier("operator new", 0x80001000) == "operator_new");
  CHECK(sanitize_identifier("new", 0x80001000) == "new_80001000");
  CHECK(sanitize_identifier("main", 0x80001000) == "main_80001000");
  CHECK(sanitize_identifier("__ct__Q23foo3BarFv", 0x80002000) == "f_ct_Q23foo3BarFv_80002000");
  CHECK(sanitize_identifier("_Unwind", 0x80002000) == "f_Unwind_80002000");
}

TEST_CASE("address classification") {
  CHECK(classify_address(0x80001000) == AccessKind::Ram);
  CHECK(classify_address(0xC0001000) == AccessKind::Ram);
  CHECK(classify_address(0xE0000010) == AccessKind::Ram);
  CHECK(classify_address(0xCC003000) == AccessKind::Mmio);
  CHECK(classify_address(0xCC008000) == AccessKind::GatherPipe);
  CHECK(classify_address(0xCC008FFC) == AccessKind::GatherPipe);
  CHECK(classify_address(0x00001000) == AccessKind::Checked);
  CHECK(classify_address(0x90000000) == AccessKind::Checked);
}
