// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal PNG writer for screenshots and frame captures: RGBA8, each row
// with the PNG filter that suits it best, compressed with a small deflate
// encoder of its own (LZ77 matches, dynamic Huffman codes), so it needs no
// compression library.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>

#include "video/renderer.h"

namespace video {

namespace {

u32 crc32(const u8* p, std::size_t n, u32 crc = 0) {
  static const std::array<u32, 256> table = [] {
    std::array<u32, 256> t{};
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::vector<u8>& v, u32 x) {
  v.push_back(static_cast<u8>(x >> 24));
  v.push_back(static_cast<u8>(x >> 16));
  v.push_back(static_cast<u8>(x >> 8));
  v.push_back(static_cast<u8>(x));
}

void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) {
  put32(out, static_cast<u32>(data.size()));
  std::vector<u8> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  put32(out, crc32(body.data(), body.size()));
}

// ---- Deflate (RFC 1951) ----------------------------------------------------------------------

class BitWriter {
 public:
  explicit BitWriter(std::vector<u8>& out) : out_(out) {}
  void put(u32 bits, u32 count) {  // least significant bit first
    buffer_ |= u64{bits} << used_;
    used_ += count;
    while (used_ >= 8) {
      out_.push_back(static_cast<u8>(buffer_));
      buffer_ >>= 8;
      used_ -= 8;
    }
  }
  // A Huffman code (most significant bit first).
  void code(u32 code, u32 length) {
    u32 reversed = 0;
    for (u32 i = 0; i < length; ++i) reversed |= ((code >> i) & 1) << (length - 1 - i);
    put(reversed, length);
  }
  void flush() {
    if (used_) out_.push_back(static_cast<u8>(buffer_));
    buffer_ = 0, used_ = 0;
  }

 private:
  std::vector<u8>& out_;
  u64 buffer_ = 0;
  u32 used_ = 0;
};

constexpr u16 kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr u8 kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr u16 kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                               193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr u8 kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// A literal (length 0) or a match.
struct Token {
  u16 length;  // 0 for a literal
  u16 value;   // the literal, or the distance
};

// Code lengths for `freqs`, at most `limit` bits (frequencies are halved
// until the tree fits).
std::vector<u8> code_lengths(std::vector<u32> freqs, u32 limit) {
  const std::size_t n = freqs.size();
  std::vector<u8> lengths(n, 0);
  for (;;) {
    struct Node {
      u32 freq;
      int left, right;
    };
    std::vector<Node> nodes;
    using Item = std::pair<u32, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
    for (std::size_t i = 0; i < n; ++i) {
      if (!freqs[i]) continue;
      nodes.push_back({freqs[i], -1, static_cast<int>(i)});
      heap.push({freqs[i], static_cast<int>(nodes.size() - 1)});
    }
    std::fill(lengths.begin(), lengths.end(), 0);
    if (nodes.empty()) return lengths;
    if (nodes.size() == 1) {
      lengths[static_cast<std::size_t>(nodes[0].right)] = 1;
      return lengths;
    }
    while (heap.size() > 1) {
      const Item a = heap.top();
      heap.pop();
      const Item b = heap.top();
      heap.pop();
      nodes.push_back({a.first + b.first, a.second, b.second});
      heap.push({a.first + b.first, static_cast<int>(nodes.size() - 1)});
    }
    u32 longest = 0;
    std::vector<std::pair<int, u32>> stack = {{heap.top().second, 0}};
    while (!stack.empty()) {
      const auto [node, depth] = stack.back();
      stack.pop_back();
      if (nodes[static_cast<std::size_t>(node)].left < 0) {
        lengths[static_cast<std::size_t>(nodes[static_cast<std::size_t>(node)].right)] = static_cast<u8>(depth);
        longest = std::max(longest, depth);
      } else {
        stack.push_back({nodes[static_cast<std::size_t>(node)].left, depth + 1});
        stack.push_back({nodes[static_cast<std::size_t>(node)].right, depth + 1});
      }
    }
    if (longest <= limit) return lengths;
    for (u32& f : freqs)
      if (f) f = (f + 1) / 2;
  }
}

// Canonical codes for code lengths.
std::vector<u16> canonical_codes(const std::vector<u8>& lengths) {
  u16 count[16] = {}, next[16] = {};
  for (u8 l : lengths) ++count[l];
  count[0] = 0;
  u16 code = 0;
  for (u32 bits = 1; bits < 16; ++bits) {
    code = static_cast<u16>((code + count[bits - 1]) << 1);
    next[bits] = code;
  }
  std::vector<u16> codes(lengths.size(), 0);
  for (std::size_t i = 0; i < lengths.size(); ++i)
    if (lengths[i]) codes[i] = next[lengths[i]]++;
  return codes;
}

u32 length_symbol(u32 length) {
  u32 i = 28;
  while (kLengthBase[i] > length) --i;
  return i;
}

u32 dist_symbol(u32 dist) {
  u32 i = 29;
  while (kDistBase[i] > dist) --i;
  return i;
}

void write_block(BitWriter& w, const std::vector<Token>& tokens, bool last) {
  std::vector<u32> lit_freq(286, 0), dist_freq(30, 0);
  for (const Token& t : tokens) {
    if (!t.length) {
      ++lit_freq[t.value];
    } else {
      ++lit_freq[257 + length_symbol(t.length)];
      ++dist_freq[dist_symbol(t.value)];
    }
  }
  lit_freq[256] = 1;  // end of block
  if (std::all_of(dist_freq.begin(), dist_freq.end(), [](u32 f) { return f == 0; })) dist_freq[0] = 1;
  const std::vector<u8> lit_len = code_lengths(lit_freq, 15), dist_len = code_lengths(dist_freq, 15);
  const std::vector<u16> lit_code = canonical_codes(lit_len), dist_code = canonical_codes(dist_len);
  u32 hlit = 286, hdist = 30;
  while (hlit > 257 && !lit_len[hlit - 1]) --hlit;
  while (hdist > 1 && !dist_len[hdist - 1]) --hdist;

  // The code lengths, run-length coded (16: repeat the last 3-6 times, 17:
  // 3-10 zeros, 18: 11-138 zeros), with their own Huffman code.
  std::vector<u8> all(lit_len.begin(), lit_len.begin() + hlit);
  all.insert(all.end(), dist_len.begin(), dist_len.begin() + hdist);
  struct Run {
    u8 symbol, extra;
  };
  std::vector<Run> runs;
  for (std::size_t i = 0; i < all.size();) {
    std::size_t j = i;
    while (j < all.size() && all[j] == all[i]) ++j;
    std::size_t n = j - i;
    if (all[i] == 0) {
      while (n >= 11) {
        const std::size_t k = std::min<std::size_t>(n, 138);
        runs.push_back({18, static_cast<u8>(k - 11)});
        n -= k;
      }
      if (n >= 3) runs.push_back({17, static_cast<u8>(n - 3)}), n = 0;
      while (n--) runs.push_back({0, 0});
    } else {
      runs.push_back({all[i], 0});
      --n;
      while (n >= 3) {
        const std::size_t k = std::min<std::size_t>(n, 6);
        runs.push_back({16, static_cast<u8>(k - 3)});
        n -= k;
      }
      while (n--) runs.push_back({all[i], 0});
    }
    i = j;
  }
  std::vector<u32> cl_freq(19, 0);
  for (const Run& r : runs) ++cl_freq[r.symbol];
  const std::vector<u8> cl_len = code_lengths(cl_freq, 7);
  const std::vector<u16> cl_code = canonical_codes(cl_len);
  static constexpr u8 kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  u32 hclen = 19;
  while (hclen > 4 && !cl_len[kOrder[hclen - 1]]) --hclen;

  w.put(last ? 1 : 0, 1);
  w.put(2, 2);  // dynamic Huffman codes
  w.put(hlit - 257, 5);
  w.put(hdist - 1, 5);
  w.put(hclen - 4, 4);
  for (u32 i = 0; i < hclen; ++i) w.put(cl_len[kOrder[i]], 3);
  for (const Run& r : runs) {
    w.code(cl_code[r.symbol], cl_len[r.symbol]);
    if (r.symbol == 16) w.put(r.extra, 2);
    else if (r.symbol == 17) w.put(r.extra, 3);
    else if (r.symbol == 18) w.put(r.extra, 7);
  }
  for (const Token& t : tokens) {
    if (!t.length) {
      w.code(lit_code[t.value], lit_len[t.value]);
      continue;
    }
    const u32 ls = length_symbol(t.length), ds = dist_symbol(t.value);
    w.code(lit_code[257 + ls], lit_len[257 + ls]);
    if (kLengthExtra[ls]) w.put(t.length - kLengthBase[ls], kLengthExtra[ls]);
    w.code(dist_code[ds], dist_len[ds]);
    if (kDistExtra[ds]) w.put(t.value - kDistBase[ds], kDistExtra[ds]);
  }
  w.code(lit_code[256], lit_len[256]);
}

// A zlib stream (RFC 1950) of `data`.
std::vector<u8> zlib_compress(const std::vector<u8>& data) {
  std::vector<u8> out = {0x78, 0x9C};
  BitWriter w(out);
  constexpr u32 kWindow = 32768, kHashBits = 15, kMaxChain = 24;
  std::vector<s32> head(std::size_t{1} << kHashBits, -1), prev(kWindow, -1);
  auto hash = [&](std::size_t i) {
    return ((u32{data[i]} << 10) ^ (u32{data[i + 1]} << 5) ^ data[i + 2]) & ((1u << kHashBits) - 1);
  };
  std::vector<Token> tokens;
  tokens.reserve(65536);
  const std::size_t n = data.size();
  for (std::size_t i = 0; i < n;) {
    u32 best_len = 0, best_dist = 0;
    if (i + 3 <= n) {
      const u32 h = hash(i);
      s32 candidate = head[h];
      for (u32 chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
        const std::size_t c = static_cast<std::size_t>(candidate);
        if (i - c > kWindow - 1) break;
        const std::size_t max = std::min<std::size_t>(258, n - i);
        std::size_t len = 0;
        while (len < max && data[c + len] == data[i + len]) ++len;
        if (len > best_len) {
          best_len = static_cast<u32>(len);
          best_dist = static_cast<u32>(i - c);
          if (len == max) break;
        }
        candidate = prev[c % kWindow];
      }
    }
    const std::size_t step = best_len >= 3 ? best_len : 1;
    if (best_len >= 3) tokens.push_back({static_cast<u16>(best_len), static_cast<u16>(best_dist)});
    else tokens.push_back({0, data[i]});
    for (std::size_t k = 0; k < step; ++k, ++i) {
      if (i + 3 > n) continue;
      const u32 h = hash(i);
      prev[i % kWindow] = head[h];
      head[h] = static_cast<s32>(i);
    }
    if (tokens.size() >= 65536) {
      write_block(w, tokens, false);
      tokens.clear();
    }
  }
  write_block(w, tokens, true);
  w.flush();
  u32 a = 1, b = 0;
  for (u8 c : data) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(out, (b << 16) | a);
  return out;
}

u8 paeth(u8 a, u8 b, u8 c) {
  const int p = int{a} + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

bool write_png(const std::string& path, const Image& image) {
  if (image.width == 0 || image.height == 0) return false;
  // Each row with the filter (none, sub, up, average, Paeth) whose output
  // looks smallest (the sum of its bytes taken as signed).
  const std::size_t stride = std::size_t{image.width} * 4;
  std::vector<u8> raw;
  raw.reserve((stride + 1) * image.height);
  std::vector<u8> candidate[5];
  for (auto& c : candidate) c.resize(stride);
  for (u32 y = 0; y < image.height; ++y) {
    const u8* row = image.rgba.data() + y * stride;
    const u8* up = y ? row - stride : nullptr;
    for (std::size_t i = 0; i < stride; ++i) {
      const u8 a = i >= 4 ? row[i - 4] : 0, b = up ? up[i] : 0, c = up && i >= 4 ? up[i - 4] : 0;
      candidate[0][i] = row[i];
      candidate[1][i] = static_cast<u8>(row[i] - a);
      candidate[2][i] = static_cast<u8>(row[i] - b);
      candidate[3][i] = static_cast<u8>(row[i] - ((int{a} + b) >> 1));
      candidate[4][i] = static_cast<u8>(row[i] - paeth(a, b, c));
    }
    int best = 0;
    u64 best_cost = ~0ull;
    for (int f = 0; f < 5; ++f) {
      u64 cost = 0;
      for (u8 v : candidate[f]) cost += static_cast<u64>(std::abs(static_cast<int>(static_cast<s8>(v))));
      if (cost < best_cost) best_cost = cost, best = f;
    }
    raw.push_back(static_cast<u8>(best));
    raw.insert(raw.end(), candidate[best].begin(), candidate[best].end());
  }

  std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<u8> ihdr;
  put32(ihdr, image.width);
  put32(ihdr, image.height);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", zlib_compress(raw));
  chunk(out, "IEND", {});

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

}  // namespace video
