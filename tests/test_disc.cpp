// SPDX-License-Identifier: GPL-3.0-or-later
//
// Disc images: SHA-1 (known answers from FIPS 180), finding the executable,
// and files by their path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
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

TEST_CASE("files are found by their path in the disc's file system") {
  // Root: a.bin, dir/ (b.bin), c.bin. Entries at 0x1000, names after them.
  std::vector<gekko::u8> image(0x3000, 0);
  image[0] = 'G', image[1] = 'T', image[2] = 'S', image[3] = 'T', image[4] = '0', image[5] = '1';
  put_be32(image, 0x1C, disc::GC_MAGIC);
  const char names[] = "a.bin\0dir\0b.bin\0c.bin";
  const gekko::u32 fst = 0x1000, count = 5, name_table = fst + count * 12;
  put_be32(image, 0x424, fst);
  put_be32(image, 0x428, count * 12 + sizeof(names));
  auto entry = [&](gekko::u32 i, bool dir, gekko::u32 name, gekko::u32 a, gekko::u32 b) {
    put_be32(image, fst + i * 12, (dir ? 0x01000000u : 0) | name);
    put_be32(image, fst + i * 12 + 4, a);
    put_be32(image, fst + i * 12 + 8, b);
  };
  entry(0, true, 0, 0, count);        // root: 5 entries
  entry(1, false, 0, 0x2000, 3);      // a.bin
  entry(2, true, 6, 0, 4);            // dir/, its entries end before 4
  entry(3, false, 10, 0x2010, 2);     // dir/b.bin
  entry(4, false, 16, 0x2020, 4);     // c.bin
  std::copy(std::begin(names), std::end(names), image.begin() + name_table);
  for (int i = 0; i < 0x30; ++i) image[0x2000 + i] = static_cast<gekko::u8>(0x80 + i);

  const auto path = std::filesystem::temp_directory_path() / "opengp_test_fst.iso";
  {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f);
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);
  }
  {
    disc::Image img;
    REQUIRE(img.open(path.string(), nullptr));
    std::vector<gekko::u8> data;
    REQUIRE(img.read_file("a.bin", &data));
    CHECK(data == std::vector<gekko::u8>{0x80, 0x81, 0x82});
    REQUIRE(img.read_file("dir/b.bin", &data));
    CHECK(data == std::vector<gekko::u8>{0x90, 0x91});
    REQUIRE(img.read_file("c.bin", &data));
    CHECK(data == std::vector<gekko::u8>{0xA0, 0xA1, 0xA2, 0xA3});
    CHECK_FALSE(img.read_file("b.bin", &data));      // only inside dir/
    CHECK_FALSE(img.read_file("dir", &data));        // a directory
    CHECK_FALSE(img.read_file("dir/c.bin", &data));  // c.bin is after dir/
    CHECK_FALSE(img.read_file("missing", &data));
  }
  std::filesystem::remove(path);
}

TEST_CASE("a preloaded image reads the same from memory") {
  // Over two of the preload's 4 MiB chunks, with a pattern that differs at
  // every offset.
  std::vector<gekko::u8> image((9 << 20) + 123);
  for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<gekko::u8>(i * 7 + (i >> 11));
  image[0] = 'G', image[1] = 'T', image[2] = 'S', image[3] = 'T', image[4] = '0', image[5] = '1';
  put_be32(image, 0x1C, disc::GC_MAGIC);

  const auto path = std::filesystem::temp_directory_path() / "opengp_test_preload.iso";
  {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    REQUIRE(f);
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);
  }
  {
    disc::Image img;
    REQUIRE(img.open(path.string(), nullptr));
    img.preload();
    for (int i = 0; i < 500 && img.preloaded() < img.size(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(img.preloaded() == img.size());
    std::vector<gekko::u8> buf(5 << 20);
    for (gekko::u64 offset : {gekko::u64{0}, gekko::u64{0x123457}, gekko::u64{(4 << 20) - 100}, gekko::u64{(9 << 20) - 5000}}) {
      const gekko::u64 n = std::min<gekko::u64>(buf.size(), image.size() - offset);
      REQUIRE(img.read(offset, buf.data(), n));
      CHECK(std::equal(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n), image.begin() + static_cast<std::ptrdiff_t>(offset)));
    }
    // Past the end: the rest reads as zero, and the read says so.
    gekko::u8 tail[8];
    CHECK_FALSE(img.read(image.size() - 3, tail, sizeof(tail)));
    CHECK(std::equal(tail, tail + 3, image.end() - 3));
    CHECK(tail[3] == 0);
    CHECK(tail[7] == 0);
    // The game ID override still applies to reads from memory.
    img.set_game_id("GZZZ01");
    gekko::u8 id[6];
    REQUIRE(img.read(0, id, sizeof(id)));
    CHECK(std::string(reinterpret_cast<const char*>(id), 6) == "GZZZ01");
  }
  {
    // Closing the image while it is still loading stops the preload.
    disc::Image img;
    REQUIRE(img.open(path.string(), nullptr));
    img.preload();
  }
  std::filesystem::remove(path);
}
