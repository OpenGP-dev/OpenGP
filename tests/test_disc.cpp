// SPDX-License-Identifier: GPL-3.0-or-later
//
// Disc images: SHA-1 (known answers from FIPS 180) and finding the executable.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "disc/disc.h"
#include "disc/sha1.h"

namespace {

std::string sha1_hex(const std::string& s) { return disc::to_hex(disc::sha1(s.data(), s.size())); }

void put_be32(std::vector<gekko::u8>& v, std::size_t at, gekko::u32 x) {
  for (int i = 0; i < 4; ++i) v[at + i] = static_cast<gekko::u8>(x >> (24 - 8 * i));
}

}  // namespace

TEST_CASE("SHA-1 known answers") {
  CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha1_hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  // Lengths around the padding boundary.
  CHECK(sha1_hex(std::string(55, 'x')) == "cef734ba81a024479e09eb5a75b6ddae62e6abf1");
  CHECK(sha1_hex(std::string(56, 'x')) == "901305367c259952f4e7af8323f480d59f81335b");
  CHECK(sha1_hex(std::string(64, 'a')) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
  CHECK(sha1_hex(std::string(119, 'y')) == "25929d08d28168361a95aa00ff2996fc221af2aa");
}

TEST_CASE("the executable is found from the disc header and its section table") {
  // A disc header with the GameCube magic and the DOL at 0x2000; the DOL has
  // a text section at 0x100 (0x40 bytes) and a data section at 0x140 (0x20).
  std::vector<gekko::u8> image(0x3000, 0);
  image[0] = 'G', image[1] = 'T', image[2] = 'S', image[3] = 'T', image[4] = '0', image[5] = '1';
  put_be32(image, 0x1C, disc::GC_MAGIC);
  put_be32(image, 0x420, 0x2000);
  put_be32(image, 0x2000 + 0x00, 0x100);   // text 0 offset
  put_be32(image, 0x2000 + 0x1C, 0x140);   // data 0 offset
  put_be32(image, 0x2000 + 0x90, 0x40);    // text 0 size
  put_be32(image, 0x2000 + 0xAC, 0x20);    // data 0 size
  for (int i = 0; i < 0x60; ++i) image[0x2100 + i] = static_cast<gekko::u8>(i + 1);

  const auto path = std::filesystem::temp_directory_path() / "opengp_test_disc.iso";
  {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f);
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);
  }
  {
    disc::Image img;
    REQUIRE(img.open(path.string(), nullptr));
    CHECK(img.game_id() == "GTST01");
    std::vector<gekko::u8> dol;
    REQUIRE(img.read_dol(&dol));
    CHECK(dol.size() == 0x160);
    CHECK(std::vector<gekko::u8>(dol.begin(), dol.end()) ==
          std::vector<gekko::u8>(image.begin() + 0x2000, image.begin() + 0x2160));
  }
  std::filesystem::remove(path);
}
