// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-256 (FIPS 180-4) of a file, to check a downloaded toolchain.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace installer {

class Sha256 {
 public:
  void update(const void* data, std::size_t size);
  std::string hex();  // finishes; lowercase hex

 private:
  void block(const std::uint8_t* p);

  std::uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t buf_[64] = {};
  std::size_t buffered_ = 0;
  std::uint64_t length_ = 0;
};

// "" if the file cannot be read.
std::string sha256_file(const std::string& path);

}  // namespace installer
