// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "os/utf8.h"

#include <filesystem>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace recomp {

namespace {

namespace fs = std::filesystem;

bool get_u32(const toml::node_view<const toml::node>& n, u32* out) {
  if (auto v = n.value<int64_t>(); v && *v >= 0 && *v <= 0xFFFFFFFFLL) {
    *out = static_cast<u32>(*v);
    return true;
  }
  return false;
}

std::string resolve(const fs::path& dir, const std::string& p) {
  if (p.empty()) return p;
  const fs::path path = os::utf8_path(p);
  return path.is_absolute() ? p : os::utf8((dir / path).lexically_normal());
}

}  // namespace

bool Config::load(const std::string& path, std::string* error) {
  auto result = toml::parse_file(path);
  if (!result) {
    if (error) {
      const auto& e = result.error();
      *error = path + ":" + std::to_string(e.source().begin.line) + ": " +
               std::string(e.description());
    }
    return false;
  }
  const toml::table& t = result.table();
  const fs::path dir = os::utf8_path(path).parent_path();
  auto fail = [&](const std::string& msg) {
    if (error) *error = path + ": " + msg;
    return false;
  };

  dol_path = resolve(dir, t["input"]["dol"].value_or(std::string()));
  symbols_path = resolve(dir, t["input"]["symbols"].value_or(std::string()));
  output_directory = t["output"]["directory"].value_or(output_directory);
  if (auto n = t["output"]["file_count"]; n && !get_u32(n, &file_count))
    return fail("output.file_count must be a positive integer");
  if (file_count == 0) return fail("output.file_count must be at least 1");

  if (auto n = t["analysis"]["sda_base"]) {
    u32 v;
    if (!get_u32(n, &v)) return fail("analysis.sda_base must be an address");
    sda_base = v;
  }
  if (auto n = t["analysis"]["sda2_base"]) {
    u32 v;
    if (!get_u32(n, &v)) return fail("analysis.sda2_base must be an address");
    sda2_base = v;
  }
  interrupt_checks = t["analysis"]["interrupt_checks"].value_or(interrupt_checks);
  if (auto* arr = t["analysis"]["exception_return"].as_array()) {
    for (const auto& el : *arr) {
      const auto name = el.value<std::string>();
      if (!name) return fail("analysis.exception_return must be a list of function names");
      exception_return.insert(*name);
    }
  }

  if (auto* arr = t["function"].as_array()) {
    for (const auto& el : *arr) {
      const toml::table* ft = el.as_table();
      if (!ft) return fail("[[function]] entries must be tables");
      FunctionOverride f;
      const auto view = toml::node_view<const toml::node>(ft);
      if (!get_u32(view["address"], &f.address) || !get_u32(view["size"], &f.size))
        return fail("[[function]] needs address and size");
      f.name = view["name"].value_or(std::string());
      functions.push_back(f);
    }
  }
  if (auto* arr = t["jump_table"].as_array()) {
    for (const auto& el : *arr) {
      const toml::table* jt = el.as_table();
      if (!jt) return fail("[[jump_table]] entries must be tables");
      JumpTableOverride j;
      const auto view = toml::node_view<const toml::node>(jt);
      if (!get_u32(view["bctr"], &j.bctr) || !get_u32(view["table"], &j.table) ||
          !get_u32(view["count"], &j.count))
        return fail("[[jump_table]] needs bctr, table and count");
      jump_tables.push_back(j);
    }
  }
  if (auto* arr = t["patch"].as_array()) {
    for (const auto& el : *arr) {
      const toml::table* pt = el.as_table();
      if (!pt) return fail("[[patch]] entries must be tables");
      const auto view = toml::node_view<const toml::node>(pt);
      u32 address, value;
      if (!get_u32(view["address"], &address) || !get_u32(view["value"], &value))
        return fail("[[patch]] needs address and value");
      patches[address] = value;
    }
  }
  if (auto* arr = t["hook"].as_array()) {
    for (const auto& el : *arr) {
      const toml::table* ht = el.as_table();
      if (!ht) return fail("[[hook]] entries must be tables");
      const auto view = toml::node_view<const toml::node>(ht);
      Hook h;
      if (!get_u32(view["address"], &h.address)) return fail("[[hook]] needs an address");
      h.function = view["function"].value_or(std::string());
      if (h.function.empty()) return fail("[[hook]] needs a function name");
      h.skip = view["skip"].value_or(false);
      h.return_after = view["return"].value_or(false);
      hooks[h.address] = h;
    }
  }
  return true;
}

}  // namespace recomp
