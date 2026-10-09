// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small baseline JPEG encoder for the camera's pictures: YCbCr 4:2:0 with
// the standard Huffman tables, each table in its own segment. That is what
// the games' JPEG decoder reads (it takes baseline JPEG only, with 4:4:4,
// 4:2:2 or 4:2:0 sampling).
#pragma once

#include <vector>

#include "gekko/types.h"

namespace camera {

using namespace gekko;

// Encodes an RGB picture (3 bytes a pixel, rows top to bottom) at `quality`
// (1-100, as libjpeg's).
std::vector<u8> encode_jpeg(const u8* rgb, int width, int height, int quality = 85);

}  // namespace camera
