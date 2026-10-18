// SPDX-License-Identifier: GPL-3.0-or-later
//
// Paths with characters from other scripts (os/utf8.h). On Windows, a path
// passed as a std::string to std::filesystem, fopen or a file stream is read
// in the system's code page: a player whose folder is named "José" or "日本"
// could not install or start the games. Everything that opens a file is
// checked here in such a folder.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app/paths.h"
#include "app/settings.h"
#include "disc/disc.h"
#include "dol.h"
#include "os/atomic_file.h"
#include "os/reboot.h"
#include "os/utf8.h"
#include "symbols.h"
#include "triforce/card_folder.h"
#include "test_temp.h"

namespace fs = std::filesystem;

namespace {

std::string u8s(const char8_t* s) { return std::string(reinterpret_cast<const char*>(s)); }

// A fresh folder whose name has Latin, Cyrillic and CJK characters (none of
// them in one Windows code page).
fs::path strange_dir(const char* test) {
  const fs::path dir = opengp_test::temp_dir(test) / os::utf8_path(u8s(u8"José Ж 日本"));
  fs::create_directories(dir);
  return dir;
}

void put_be32(std::vector<gekko::u8>& v, std::size_t at, gekko::u32 x) {
  for (int i = 0; i < 4; ++i) v[at + i] = static_cast<gekko::u8>(x >> (24 - 8 * i));
}

}  // namespace

TEST_CASE("UTF-8 text and paths convert both ways") {
  const std::string name = u8s(u8"café/Жук/日本.iso");
  CHECK(os::utf8(os::utf8_path(name)) == name);
  CHECK(os::utf8(os::utf8_path(name).filename()) == u8s(u8"日本.iso"));
}

TEST_CASE("files open in a folder named in other scripts") {
  const fs::path dir = strange_dir("files");
  const std::string file = os::utf8(dir / u8"é.bin");
  REQUIRE(os::write_file_atomically(file, "abc", 3));
  std::FILE* f = os::fopen_utf8(file, "rb");
  REQUIRE(f);
  char buf[4] = {};
  CHECK(std::fread(buf, 1, 4, f) == 3);
  std::fclose(f);
  CHECK(std::string(buf) == "abc");
  CHECK(fs::exists(dir / u8"é.bin"));
}

TEST_CASE("settings, reboot files and cards in such a folder") {
  const fs::path dir = strange_dir("user");

  app::Settings s;
  s.volume = 42;
  std::string error;
  const std::string settings = os::utf8(dir / "settings.toml");
  REQUIRE_MESSAGE(app::save_settings(settings, s, &error), error);
  app::Settings loaded;
  REQUIRE_MESSAGE(app::load_settings(settings, &loaded, &error), error);
  CHECK(loaded.volume == 42);

  os::Reboot r;
  r.reset_code = 7;
  r.region.assign(16, 1);
  const std::string reboot = os::utf8(dir / "reboot.bin");
  REQUIRE(os::save_reboot(reboot, r));
  os::Reboot back;
  REQUIRE(os::load_reboot(reboot, &back));
  CHECK(back.reset_code == 7);

  const fs::path cards_dir = dir / "cards";
  fs::create_directories(cards_dir);
  std::ofstream(cards_dir / u8"マリオ.bin", std::ios::binary) << std::string(69, 'x');
  triforce::CardFolder cards(os::utf8(cards_dir));
  CHECK(cards.list() == std::vector<std::string>{u8s(u8"マリオ")});
  CHECK(cards.exists(u8s(u8"マリオ")));

  REQUIRE_MESSAGE(app::remember_image(dir, "GTST01", os::utf8(dir / u8"日本.iso"), &error), error);
  CHECK(app::remembered_image(dir, "GTST01") == os::utf8(dir / u8"日本.iso"));
}

TEST_CASE("a disc image in such a folder") {
  std::vector<gekko::u8> image(0x3000, 0);
  image[0] = 'G', image[1] = 'T', image[2] = 'S', image[3] = 'T', image[4] = '0', image[5] = '1';
  put_be32(image, 0x1C, disc::GC_MAGIC);
  put_be32(image, 0x420, 0x2000);
  put_be32(image, 0x2000 + 0x00, 0x100);
  put_be32(image, 0x2000 + 0x90, 0x40);
  const std::string path = os::utf8(strange_dir("disc") / u8"マリオカート.iso");
  REQUIRE(os::write_file_atomically(path, image.data(), image.size()));
  disc::Image img;
  std::string error;
  REQUIRE_MESSAGE(img.open(path, &error), error);
  CHECK(img.game_id() == "GTST01");
  std::vector<gekko::u8> dol;
  CHECK(img.read_dol(&dol));
}

TEST_CASE("the recompiler's inputs in such a folder") {
  const fs::path dir = strange_dir("recomp");
  std::string error;
  const std::string bin = os::utf8(dir / "main.dol");
  REQUIRE_MESSAGE(recomp::write_file(bin, "\x01\x02", 2, &error), error);
  std::vector<gekko::u8> data;
  REQUIRE_MESSAGE(recomp::read_file(bin, &data, &error), error);
  CHECK(data.size() == 2);

  const std::string symbols = os::utf8(dir / "symbols.txt");
  const std::string text = "fn_80003100 = .text:0x80003100; // type:function size:0x4\n";
  REQUIRE(os::write_file_atomically(symbols, text.data(), text.size()));
  recomp::SymbolFile file;
  REQUIRE_MESSAGE(file.load(symbols, &error), error);
}
