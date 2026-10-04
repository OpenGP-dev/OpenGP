// SPDX-License-Identifier: GPL-3.0-or-later
//
// DOL executable format (GameCube / Triforce main executables).
#pragma once

#include <string>
#include <vector>

#include "gekko/types.h"

namespace recomp {

using namespace gekko;

struct DolSection {
  u32 file_offset = 0;
  u32 address = 0;
  u32 size = 0;
  bool text = false;
  int index = 0;  // index within its kind (text0..6, data0..10)
};

class Dol {
 public:
  static constexpr int kTextCount = 7;
  static constexpr int kDataCount = 11;
  static constexpr u32 kHeaderSize = 0x100;

  bool parse(std::vector<u8> bytes, std::string* error);
  bool load(const std::string& path, std::string* error);

  const std::vector<DolSection>& sections() const { return sections_; }
  u32 entry_point() const { return entry_; }
  u32 bss_address() const { return bss_address_; }
  u32 bss_size() const { return bss_size_; }

  // Section containing [addr, addr+size), or nullptr.
  const DolSection* section_at(u32 addr, u32 size = 1) const;
  bool is_text(u32 addr) const {
    const DolSection* s = section_at(addr, 4);
    return s && s->text;
  }
  // Pointer to the file bytes for a guest address range, or nullptr.
  const u8* data_at(u32 addr, u32 size) const;
  bool read_u32(u32 addr, u32* out) const;

  const std::vector<u8>& bytes() const { return bytes_; }

 private:
  std::vector<u8> bytes_;
  std::vector<DolSection> sections_;
  u32 entry_ = 0;
  u32 bss_address_ = 0;
  u32 bss_size_ = 0;
};

// Builds DOL files (used by the test fixture generator).
class DolBuilder {
 public:
  void add_text(u32 address, std::vector<u8> data);
  void add_data(u32 address, std::vector<u8> data);
  void set_bss(u32 address, u32 size) {
    bss_address_ = address;
    bss_size_ = size;
  }
  void set_entry(u32 entry) { entry_ = entry; }
  std::vector<u8> build() const;

 private:
  struct Pending {
    u32 address;
    std::vector<u8> data;
  };
  std::vector<Pending> text_, data_;
  u32 bss_address_ = 0, bss_size_ = 0, entry_ = 0;
};

bool read_file(const std::string& path, std::vector<u8>* out, std::string* error);
bool write_file(const std::string& path, const void* data, std::size_t size, std::string* error);

}  // namespace recomp
