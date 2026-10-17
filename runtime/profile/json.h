// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small JSON writer for profiles: one object, built field by field.
//
//   profile::Json().add("frame", 12).add("scene", "race").str()  ->  {"frame":12,"scene":"race"}
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace profile {

// `s` as a JSON string, quotes included.
inline std::string json_string(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out + "\"";
}

class Json {
 public:
  Json& add(std::string_view key, std::string_view value) { return raw(key, json_string(value)); }
  Json& add(std::string_view key, const char* value) { return raw(key, value ? json_string(value) : "null"); }
  Json& add(std::string_view key, const std::string& value) { return raw(key, json_string(value)); }
  Json& add(std::string_view key, const Json& value) { return raw(key, value.str()); }
  template <typename T>
    requires std::is_arithmetic_v<T>
  Json& add(std::string_view key, T value) {
    if constexpr (std::is_same_v<T, bool>) {
      return raw(key, value ? "true" : "false");
    } else if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value)) return raw(key, "null");
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
      return raw(key, buf);
    } else {
      return raw(key, std::to_string(value));
    }
  }
  // A value that is JSON already (an array, an object).
  Json& raw(std::string_view key, std::string_view json) {
    if (!body_.empty()) body_ += ',';
    body_ += json_string(key);
    body_ += ':';
    body_ += json;
    return *this;
  }
  bool empty() const { return body_.empty(); }
  std::string str() const { return "{" + body_ + "}"; }
  // The fields without the braces, to merge into another object.
  const std::string& fields() const { return body_; }

 private:
  std::string body_;
};

// A JSON array of already formatted values.
class JsonArray {
 public:
  JsonArray& raw(std::string_view json) {
    if (!body_.empty()) body_ += ',';
    body_ += json;
    return *this;
  }
  JsonArray& add(std::string_view s) { return raw(json_string(s)); }
  JsonArray& add(const Json& j) { return raw(j.str()); }
  std::string str() const { return "[" + body_ + "]"; }

 private:
  std::string body_;
};

}  // namespace profile
