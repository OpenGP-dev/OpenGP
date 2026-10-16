// SPDX-License-Identifier: GPL-3.0-or-later
//
// Textures: telling mipmaps that make a texture fade in with distance (an
// effect) from downscaled copies (gx::fades_with_distance).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <vector>

#include "gx/texture.h"

using namespace gx;

namespace {

gekko::u32 rgba(gekko::u32 r, gekko::u32 g, gekko::u32 b, gekko::u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

// A smooth 64x64 picture and its levels, each a 2x2 box average of the one above.
std::vector<gekko::u32> box_filtered(gekko::u32 levels) {
  std::vector<gekko::u32> t;
  for (gekko::u32 y = 0; y < 64; ++y)
    for (gekko::u32 x = 0; x < 64; ++x) t.push_back(rgba(x * 4, y * 4, (x + y) * 2, 255));
  std::size_t prev = 0;
  for (gekko::u32 l = 1, w = 32; l < levels; ++l, w /= 2) {
    const std::size_t here = t.size();
    for (gekko::u32 y = 0; y < w; ++y) {
      for (gekko::u32 x = 0; x < w; ++x) {
        gekko::u32 out = 0;
        for (gekko::u32 k = 0; k < 32; k += 8) {
          auto c = [&](gekko::u32 dx, gekko::u32 dy) { return (t[prev + (2 * y + dy) * (2 * w) + 2 * x + dx] >> k) & 0xFF; };
          out |= ((c(0, 0) + c(1, 0) + c(0, 1) + c(1, 1) + 2) / 4) << k;
        }
        t.push_back(out);
      }
    }
    prev = here;
  }
  return t;
}

}  // namespace

TEST_CASE("downscaled mipmaps are not an effect") {
  const auto t = box_filtered(7);
  CHECK_FALSE(fades_with_distance(t.data(), 64, 64, 7));
  CHECK_FALSE(fades_with_distance(t.data(), 64, 64, 1));
}

TEST_CASE("a transparent level 0 over visible arcs is an effect (GP 2's rainbows)") {
  // Level 0 invisible everywhere; level 1 opaque on one row in eight.
  std::vector<gekko::u32> t(128 * 128, rgba(145, 130, 116, 0));
  for (gekko::u32 y = 0; y < 64; ++y)
    for (gekko::u32 x = 0; x < 64; ++x) t.push_back(rgba(145, 130, 116, y % 8 == 0 ? 255 : 0));
  CHECK(fades_with_distance(t.data(), 128, 128, 2));
}

TEST_CASE("levels compressed apart, or of another colour, are not flagged") {
  auto t = box_filtered(7);
  for (std::size_t i = 64 * 64; i < 64 * 64 + 32 * 32; ++i) t[i] = rgba(255, 0, 0, 255);
  CHECK_FALSE(fades_with_distance(t.data(), 64, 64, 7));
}

TEST_CASE("a transparent texture with transparent levels is not flagged") {
  std::vector<gekko::u32> t(64 * 64 + 32 * 32, rgba(1, 2, 3, 0));
  CHECK_FALSE(fades_with_distance(t.data(), 64, 64, 2));
}
