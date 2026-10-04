// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-1 (FIPS 180-4), to check that a disc image holds the executable a build
// was recompiled from.
#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "gekko/types.h"

namespace disc {

using Sha1Digest = std::array<gekko::u8, 20>;

Sha1Digest sha1(const void* data, std::size_t size);

// Lowercase hex, as sha1sum prints it.
std::string to_hex(const Sha1Digest& digest);

}  // namespace disc
