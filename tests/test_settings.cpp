// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the player's settings (runtime/app/settings.h) and the save
// card folder (runtime/triforce/card_folder.h).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "app/settings.h"
#include "os/atomic_file.h"
#include "triforce/card_folder.h"

namespace fs = std::filesystem;

namespace {

// A fresh folder for a test's files.
fs::path temp_dir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "opengp_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

}  // namespace

TEST_CASE("settings: a missing file keeps the defaults") {
  app::Settings s;
  std::string error;
  CHECK(app::load_settings((temp_dir("missing") / "settings.toml").string(), &s, &error));
  CHECK(s == app::Settings{});
}

TEST_CASE("settings: save and load round trip") {
  const fs::path path = temp_dir("round_trip") / "settings.toml";
  app::Settings s;
  s.fullscreen = true;
  s.window_width = 1600;
  s.window_height = 900;
  s.aspect = app::AspectRatio::Wide21x9;
  s.internal_resolution = 3;
  s.vsync = false;
  s.volume = 42;
  s.free_play = true;
  s.card = "Mario's \"best\" card";
  std::string error;
  REQUIRE_MESSAGE(app::save_settings(path.string(), s, &error), error);
  app::Settings loaded;
  REQUIRE_MESSAGE(app::load_settings(path.string(), &loaded, &error), error);
  CHECK(loaded == s);
}

TEST_CASE("settings: partial files, clamping and errors") {
  const fs::path path = temp_dir("partial") / "settings.toml";
  write(path, "[video]\naspect_ratio = \"16:9\"\ninternal_resolution = 99\n[audio]\nvolume = 300\n");
  app::Settings s;
  std::string error;
  REQUIRE_MESSAGE(app::load_settings(path.string(), &s, &error), error);
  CHECK(s.aspect == app::AspectRatio::Wide16x9);
  CHECK(s.internal_resolution == 8);
  CHECK(s.volume == 100);
  CHECK(s.window_width == app::Settings{}.window_width);  // not in the file

  write(path, "[video]\naspect_ratio = \"5:4\"\n");
  CHECK_FALSE(app::load_settings(path.string(), &s, &error));
  CHECK(error.find("5:4") != std::string::npos);
  write(path, "[video\n");
  CHECK_FALSE(app::load_settings(path.string(), &s, &error));
}

TEST_CASE("settings: widescreen sides") {
  using app::AspectRatio;
  CHECK(app::widescreen_side(AspectRatio::Original, 1920, 1080) == 0);
  CHECK(app::widescreen_side(AspectRatio::Wide16x9, 0, 0) == 107);    // 854 pixels wide
  CHECK(app::widescreen_side(AspectRatio::Wide16x10, 0, 0) == 64);    // 768
  CHECK(app::widescreen_side(AspectRatio::Wide21x9, 0, 0) == 249);    // 1138
  CHECK(app::widescreen_side(AspectRatio::Auto, 1920, 1080) == 107);  // the window's shape
  CHECK(app::widescreen_side(AspectRatio::Auto, 1024, 768) == 0);
  CHECK(app::widescreen_side(AspectRatio::Auto, 800, 800) == 0);      // narrower than 4:3: letterboxed
  CHECK(app::widescreen_side(AspectRatio::Auto, 0, 0) == 0);          // no window
  CHECK(app::widescreen_side(AspectRatio::Auto, 10000, 100) == 533);  // at most 32:9
}

TEST_CASE("settings: internal resolution") {
  CHECK(app::internal_scale(2, 2160) == 2);
  CHECK(app::internal_scale(0, 480) == 1);
  CHECK(app::internal_scale(0, 720) == 2);
  CHECK(app::internal_scale(0, 1080) == 3);
  CHECK(app::internal_scale(0, 2160) == 5);
  CHECK(app::internal_scale(0, 0) == 1);
  CHECK(app::internal_scale(0, 100000) == 8);
}

TEST_CASE("atomic file writes replace the file") {
  const fs::path path = temp_dir("atomic") / "sub" / "file.bin";
  REQUIRE(os::write_file_atomically(path.string(), "first", 5));
  REQUIRE(os::write_file_atomically(path.string(), "2nd", 3));
  std::ifstream f(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(text == "2nd");
  CHECK_FALSE(fs::exists(path.string() + ".tmp"));
}

TEST_CASE("card folder") {
  const fs::path dir = temp_dir("cards");
  triforce::CardFolder cards(dir.string());
  CHECK(cards.list().empty());
  CHECK(cards.unused_name() == "Card 1");
  write(dir / "Card 1.bin", std::string(69, 'x'));
  write(dir / "Luigi.bin", std::string(69, 'y'));
  write(dir / "notes.txt", "not a card");
  CHECK(cards.list() == std::vector<std::string>{"Card 1", "Luigi"});
  CHECK(cards.unused_name() == "Card 2");
  CHECK(cards.exists("Luigi"));
  CHECK_FALSE(cards.exists("Card 2"));
  CHECK_FALSE(cards.exists(""));
  CHECK(cards.path("Luigi") == dir.string() + "/Luigi.bin");
  cards.hold("Luigi");
  CHECK(cards.held() == "Luigi");
  CHECK_FALSE(cards.inserted());
}
