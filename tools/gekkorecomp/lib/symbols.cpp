// SPDX-License-Identifier: GPL-3.0-or-later

#include "symbols.h"
#include "os/utf8.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace recomp {

namespace {

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

bool parse_number(std::string_view s, u32* out) {
  int radix = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s.remove_prefix(2);
    radix = 16;
  }
  u64 v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, radix);
  if (ec != std::errc() || ptr != s.data() + s.size() || v > 0xFFFFFFFFull) return false;
  *out = static_cast<u32>(v);
  return true;
}

const char* type_name(Symbol::Type t) {
  switch (t) {
    case Symbol::Type::Function: return "function";
    case Symbol::Type::Object: return "object";
    case Symbol::Type::Label: return "label";
    case Symbol::Type::Unknown: break;
  }
  return nullptr;
}

bool needs_quotes(const std::string& name) {
  for (char ch : name)
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '=' || ch == ';' || ch == '"')
      return true;
  return name.empty();
}

}  // namespace

bool SymbolFile::load(const std::string& path, std::string* error) {
  std::ifstream f(os::utf8_path(path));
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  if (!parse(ss.str(), error)) {
    if (error) *error = path + ": " + *error;
    return false;
  }
  return true;
}

bool SymbolFile::parse(const std::string& text, std::string* error) {
  std::istringstream in(text);
  std::string raw_line;
  int line_no = 0;
  auto fail = [&](const std::string& msg) {
    if (error) *error = "line " + std::to_string(line_no) + ": " + msg;
    return false;
  };

  while (std::getline(in, raw_line)) {
    ++line_no;
    std::string_view line = raw_line;
    if (trim(line).empty()) continue;
    if (trim(line).rfind("//", 0) == 0) continue;

    Symbol sym;
    std::size_t pos = 0;
    // Name (optionally quoted).
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    if (pos < line.size() && line[pos] == '"') {
      const std::size_t end = line.find('"', pos + 1);
      if (end == std::string_view::npos) return fail("unterminated quoted name");
      sym.name = std::string(line.substr(pos + 1, end - pos - 1));
      pos = end + 1;
    } else {
      const std::size_t eq = line.find('=', pos);
      if (eq == std::string_view::npos) return fail("expected '='");
      sym.name = trim(line.substr(pos, eq - pos));
      pos = eq;
    }
    pos = line.find('=', pos);
    if (pos == std::string_view::npos) return fail("expected '='");
    ++pos;

    // Section:address;
    const std::size_t semi = line.find(';', pos);
    if (semi == std::string_view::npos) return fail("expected ';'");
    const std::string loc = trim(line.substr(pos, semi - pos));
    const std::size_t colon = loc.rfind(':');
    if (colon == std::string::npos) {
      // Absolute symbol: "name = 0x80001234;"
      if (!parse_number(loc, &sym.address)) return fail("bad address '" + loc + "'");
    } else {
      sym.section = loc.substr(0, colon);
      if (!parse_number(loc.substr(colon + 1), &sym.address))
        return fail("bad address '" + loc + "'");
    }

    // Attributes after "//".
    const std::size_t comment = line.find("//", semi);
    if (comment != std::string_view::npos) {
      std::istringstream attrs{std::string(line.substr(comment + 2))};
      std::string tok;
      while (attrs >> tok) {
        const std::size_t c = tok.find(':');
        const std::string key = c == std::string::npos ? tok : tok.substr(0, c);
        const std::string value = c == std::string::npos ? "" : tok.substr(c + 1);
        if (key == "type") {
          if (value == "function") sym.type = Symbol::Type::Function;
          else if (value == "object") sym.type = Symbol::Type::Object;
          else if (value == "label") sym.type = Symbol::Type::Label;
        } else if (key == "size") {
          u32 size;
          if (!parse_number(value, &size)) return fail("bad size '" + value + "'");
          sym.size = size;
        } else if (key == "scope") {
          sym.scope = value;
        } else {
          sym.attributes[key] = value;
        }
      }
    }
    symbols.push_back(std::move(sym));
  }
  return true;
}

std::string SymbolFile::serialize() const {
  std::string out;
  char buf[64];
  for (const Symbol& s : symbols) {
    out += needs_quotes(s.name) ? "\"" + s.name + "\"" : s.name;
    std::snprintf(buf, sizeof(buf), "0x%08X", s.address);
    out += " = " + (s.section.empty() ? std::string() : s.section + ":") + buf + ";";
    std::string attrs;
    if (const char* t = type_name(s.type)) attrs += std::string(" type:") + t;
    if (s.size) {
      std::snprintf(buf, sizeof(buf), " size:0x%X", *s.size);
      attrs += buf;
    }
    if (!s.scope.empty()) attrs += " scope:" + s.scope;
    for (const auto& [k, v] : s.attributes) attrs += " " + k + (v.empty() ? "" : ":" + v);
    if (!attrs.empty()) out += " //" + attrs;
    out += "\n";
  }
  return out;
}

}  // namespace recomp
