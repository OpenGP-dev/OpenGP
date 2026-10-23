// SPDX-License-Identifier: GPL-3.0-or-later
//
// The installer (tools/installer): reading a package's package.toml, from this
// release and from older ones.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "installer/installer.h"
#include "test_temp.h"

namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// A package folder with one game, whose package.toml's [build] ends with `build`.
fs::path package(const char* test, const std::string& build) {
  const fs::path home = opengp_test::temp_dir(test);
  write(home / "sdk" / "package.toml",
        "version = \"1.0.0\"\ngames = [\"mkgp2\"]\n\n[build]\ncxx = [\"c++\", \"g++\", \"clang++\"]\n"
        "link_group = true\n" + build);
  write(home / "sdk" / "games" / "mkgp2" / "game.toml",
        "id = \"GGPE02\"\ntitle = \"Mario Kart Arcade GP 2\"\nexecutable = \"opengp-mkgp2\"\n");
  return home;
}

}  // namespace

TEST_CASE("a package from 1.0.0, which lists no sources, builds 1.0.0's") {
  installer::Package p;
  std::string error;
  REQUIRE(installer::load_package(package("sources_1_0_0", ""), &p, &error));
  CHECK(p.common_sources == std::vector<std::string>{"main.cpp", "arcade.cpp", "cabinet.cpp", "camera_use.cpp",
                                                     "game_log.cpp", "sdk_hle.cpp"});
  CHECK(p.game_sources == std::vector<std::string>{"patches.cpp", "status.cpp"});
  REQUIRE(p.games.size() == 1);
  CHECK(p.games[0].id == "GGPE02");
}

TEST_CASE("a package's lists of sources are the ones built") {
  installer::Package p;
  std::string error;
  REQUIRE(installer::load_package(
      package("sources_listed", "common_sources = [\"main.cpp\", \"new.cpp\"]\ngame_sources = [\"patches.cpp\"]\n"),
      &p, &error));
  CHECK(p.common_sources == std::vector<std::string>{"main.cpp", "new.cpp"});
  CHECK(p.game_sources == std::vector<std::string>{"patches.cpp"});
}

TEST_CASE("a package with an empty list of sources is refused") {
  for (const char* build : {"common_sources = []\n", "game_sources = []\n",
                            "common_sources = [\"main.cpp\"]\ngame_sources = []\n"}) {
    CAPTURE(build);
    installer::Package p;
    std::string error;
    CHECK_FALSE(installer::load_package(package("sources_empty", build), &p, &error));
    CHECK(error.ends_with("package.toml: no game sources listed"));
  }
}
