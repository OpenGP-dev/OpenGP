// SPDX-License-Identifier: GPL-3.0-or-later

#include "dol.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace recomp {

namespace {

u32 be32(const u8* p) {
  return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | u32{p[3]};
}

void put_be32(std::vector<u8>& out, std::size_t at, u32 v) {
  out[at] = static_cast<u8>(v >> 24);
  out[at + 1] = static_cast<u8>(v >> 16);
  out[at + 2] = static_cast<u8>(v >> 8);
  out[at + 3] = static_cast<u8>(v);
}

}  // namespace

bool read_file(const std::string& path, std::vector<u8>* out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const std::string& path, const void* data, std::size_t size, std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(f);
}

bool Dol::load(const std::string& path, std::string* error) {
  std::vector<u8> bytes;
  if (!read_file(path, &bytes, error)) return false;
  if (!parse(std::move(bytes), error)) {
    if (error) *error = path + ": " + *error;
    return false;
  }
  return true;
}

bool Dol::parse(std::vector<u8> bytes, std::string* error) {
  bytes_ = std::move(bytes);
  sections_.clear();
  if (bytes_.size() < kHeaderSize) {
    if (error) *error = "file too small for a DOL header";
    return false;
  }
  const u8* h = bytes_.data();
  for (int i = 0; i < kTextCount + kDataCount; ++i) {
    DolSection s;
    s.file_offset = be32(h + 0x00 + 4 * i);
    s.address = be32(h + 0x48 + 4 * i);
    s.size = be32(h + 0x90 + 4 * i);
    s.text = i < kTextCount;
    s.index = s.text ? i : i - kTextCount;
    if (s.size == 0) continue;
    if (u64{s.file_offset} + s.size > bytes_.size()) {
      if (error) *error = "section extends past end of file";
      return false;
    }
    sections_.push_back(s);
  }
  bss_address_ = be32(h + 0xD8);
  bss_size_ = be32(h + 0xDC);
  entry_ = be32(h + 0xE0);
  return true;
}

const DolSection* Dol::section_at(u32 addr, u32 size) const {
  for (const DolSection& s : sections_)
    if (addr >= s.address && u64{addr} + size <= u64{s.address} + s.size) return &s;
  return nullptr;
}

const u8* Dol::data_at(u32 addr, u32 size) const {
  const DolSection* s = section_at(addr, size);
  return s ? bytes_.data() + s->file_offset + (addr - s->address) : nullptr;
}

bool Dol::read_u32(u32 addr, u32* out) const {
  const u8* p = data_at(addr, 4);
  if (!p) return false;
  *out = be32(p);
  return true;
}

void DolBuilder::add_text(u32 address, std::vector<u8> data) {
  if (text_.size() >= Dol::kTextCount) throw std::runtime_error("too many text sections");
  text_.push_back({address, std::move(data)});
}

void DolBuilder::add_data(u32 address, std::vector<u8> data) {
  if (data_.size() >= Dol::kDataCount) throw std::runtime_error("too many data sections");
  data_.push_back({address, std::move(data)});
}

std::vector<u8> DolBuilder::build() const {
  std::vector<u8> out(Dol::kHeaderSize, 0);
  auto append = [&](int slot, const Pending& p) {
    while (out.size() % 32) out.push_back(0);
    const u32 offset = static_cast<u32>(out.size());
    out.insert(out.end(), p.data.begin(), p.data.end());
    put_be32(out, 0x00 + 4 * slot, offset);
    put_be32(out, 0x48 + 4 * slot, p.address);
    put_be32(out, 0x90 + 4 * slot, static_cast<u32>(p.data.size()));
  };
  for (std::size_t i = 0; i < text_.size(); ++i) append(static_cast<int>(i), text_[i]);
  for (std::size_t i = 0; i < data_.size(); ++i)
    append(Dol::kTextCount + static_cast<int>(i), data_[i]);
  put_be32(out, 0xD8, bss_address_);
  put_be32(out, 0xDC, bss_size_);
  put_be32(out, 0xE0, entry_);
  return out;
}

}  // namespace recomp
