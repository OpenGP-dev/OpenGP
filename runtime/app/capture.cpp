// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#include "gx/describe.h"
#include "profile/json.h"
#include "profile/symbols.h"

namespace app {

namespace fs = std::filesystem;
using namespace gekko;
using profile::Json;
using profile::JsonArray;

namespace {

std::string format(const char* fmt, auto... args) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  return buf;
}

std::string hex8(u32 v) { return format("%08X", v); }

// "Name+0x1C" for a guest address (the game's symbols).
std::string guest_function(const FunctionNames& names, u32 address) {
  auto it = names.upper_bound(address);
  if (it == names.begin()) return hex8(address);
  --it;
  return address == it->first ? it->second : format("%s+0x%X", it->second.c_str(), address - it->first);
}

// A library in the game: functions whose names start with one of its
// prefixes, and unnamed ones among them (by address).
struct Library {
  std::vector<std::string> prefixes;
  u32 lo = ~0u, hi = 0;
  Library(const FunctionNames& names, std::vector<std::string> p) : prefixes(std::move(p)) {
    for (const auto& [address, name] : names)
      if (named(name)) lo = std::min(lo, address), hi = std::max(hi, address);
  }
  bool named(const std::string& name) const {
    for (const std::string& p : prefixes)
      if (name.rfind(p, 0) == 0 || name.rfind("f_" + p, 0) == 0) return true;
    return false;
  }
  bool contains(const std::string& name) const {
    if (named(name)) return true;
    const std::size_t at = name.rfind('_');
    if (at == std::string::npos || name.size() - at != 9) return false;
    const u32 address = static_cast<u32>(std::strtoul(name.c_str() + at + 1, nullptr, 16));
    return address >= lo && address <= hi;
  }
};

// The game's GX library (the function that drew a primitive is the first on
// the stack outside it) and its engine, HAL's SysDolphin (HSD_*), which draws
// the models: the code that owns a draw is the first outside both.
struct Libraries {
  Library gx, hsd;
  explicit Libraries(const FunctionNames& names) : gx(names, {"GX", "__GX"}), hsd(names, {"HSD_", "_HSD", "hsd"}) {}
};

// The recompiled game functions on a host stack, innermost first, from the
// first outside the GX library (`library`: the innermost one inside it).
std::vector<std::string> host_guest_functions(const gx::FrameCapture::Stacks& s, std::size_t max, const Library& gx,
                                              std::string* library = nullptr) {
  std::vector<std::string> out;
  for (u8 i = 0; i < s.host_depth && out.size() < max; ++i) {
    const profile::HostSymbol symbol = profile::host_symbol(s.host[i]);
    if (!profile::is_guest_symbol(symbol.name)) continue;
    std::string name = profile::readable_symbol(symbol.name);
    if (out.empty() && gx.contains(name)) {
      if (library && library->empty()) *library = name;
      continue;
    }
    out.push_back(std::move(name));
  }
  return out;
}

std::vector<std::string> guest_functions(const FunctionNames& names, const gx::FrameCapture::Stacks& s) {
  std::vector<std::string> out;
  for (u8 i = 0; i < s.guest_depth; ++i) out.push_back(guest_function(names, s.guest[i]));
  return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
  std::string out;
  for (const std::string& s : v) out += (out.empty() ? "" : sep) + s;
  return out;
}

JsonArray json_strings(const std::vector<std::string>& v) {
  JsonArray a;
  for (const std::string& s : v) a.add(s);
  return a;
}

bool save_png(const fs::path& path, const video::Image& image) {
  return image.width && video::write_png(path.string(), image);
}

// Depths (24-bit, packed in RGB) as grey, near white, stretched over the
// range the picture uses (cleared, farthest pixels black).
video::Image depth_picture(const video::Image& raw) {
  video::Image out = raw;
  const std::size_t n = std::size_t{raw.width} * raw.height;
  auto depth = [&](std::size_t i) {
    return (u32{raw.rgba[4 * i]} << 16) | (u32{raw.rgba[4 * i + 1]} << 8) | raw.rgba[4 * i + 2];
  };
  u32 lo = 0xFFFFFF, hi = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const u32 d = depth(i);
    if (d >= 0xFFFFFF) continue;
    lo = std::min(lo, d), hi = std::max(hi, d);
  }
  for (std::size_t i = 0; i < n; ++i) {
    const u32 d = depth(i);
    u8 v = 0;
    if (d < 0xFFFFFF && hi > lo) v = static_cast<u8>(255 - (static_cast<u64>(d - lo) * 230 / (hi - lo)));
    else if (d < 0xFFFFFF) v = 255;
    out.rgba[4 * i] = out.rgba[4 * i + 1] = out.rgba[4 * i + 2] = v;
    out.rgba[4 * i + 3] = 255;
  }
  return out;
}

// "pos idx16, nrm -, col0 direct, tex0 idx16" from the vertex descriptor.
std::string vertex_format(u32 lo, u32 hi) {
  static constexpr const char* kType[4] = {"-", "direct", "idx8", "idx16"};
  std::string s = format("pos %s", kType[gx::bits(lo, 9, 2)]);
  if (gx::bits(lo, 11, 2)) s += format(", nrm %s", kType[gx::bits(lo, 11, 2)]);
  for (u32 i = 0; i < 2; ++i)
    if (gx::bits(lo, 13 + 2 * i, 2)) s += format(", col%u %s", i, kType[gx::bits(lo, 13 + 2 * i, 2)]);
  for (u32 i = 0; i < 8; ++i)
    if (gx::bits(hi, 2 * i, 2)) s += format(", tex%u %s", i, kType[gx::bits(hi, 2 * i, 2)]);
  if (gx::bits(lo, 0, 1)) s += ", posmtx idx";
  return s;
}

// A colour channel's control (XF_COLOR0_CTRL...): "material vertex, lit by 0x03, ambient register".
std::string channel(u32 c) {
  const u32 lights = gx::bits(c, 2, 4) | (gx::bits(c, 11, 4) << 4);
  std::string s = gx::bits(c, 0, 1) ? "material vertex" : "material register";
  if (gx::bits(c, 1, 1)) {
    s += format(", lit by lights %02X, ambient %s", lights, gx::bits(c, 6, 1) ? "vertex" : "register");
    static constexpr const char* kDiffuse[4] = {"none", "sign", "clamp", "?"};
    if (gx::bits(c, 9, 1)) s += gx::bits(c, 10, 1) ? ", spot" : ", specular";
    s += format(", diffuse %s", kDiffuse[gx::bits(c, 7, 2)]);
  } else {
    s += ", unlit";
  }
  return s;
}

std::string texture_file(u32 handle, const gx::FrameCapture::Texture& t) {
  return format("t%u_%08X_%s_%ux%u.png", handle, t.address, gx::texture_format_name(t.format), t.width, t.height);
}

}  // namespace

bool write_capture(const fs::path& dir, const gx::Frame& frame, const video::CaptureImages& images,
                   const FunctionNames& names, const CaptureInfo& info, std::string* error) {
  std::error_code ec;
  fs::create_directories(dir / "textures", ec);
  fs::create_directories(dir / "copies", ec);
  if (ec) {
    *error = "cannot create " + dir.string() + ": " + ec.message();
    return false;
  }
  if (!frame.capture) {
    *error = "the frame has no capture data";
    return false;
  }
  const gx::FrameCapture& cap = *frame.capture;
  const Libraries libraries(names);
  const Library& gx_library = libraries.gx;
  // The first function in a chain outside the engine.
  auto owner = [&](const std::vector<std::string>& chain) {
    for (const std::string& f : chain)
      if (!libraries.hsd.contains(f) && !libraries.gx.contains(f)) return f;
    return chain.empty() ? std::string("?") : chain.back();
  };

  // Pictures.
  save_png(dir / "screen.png", images.xfb);
  save_png(dir / "efb_end.png", images.efb_end);
  save_png(dir / "efb_start.png", images.efb_start);
  if (images.depth_end.width) {
    save_png(dir / "depth_end_raw.png", images.depth_end);
    save_png(dir / "depth_end.png", depth_picture(images.depth_end));
  }
  if (images.depth_start.width) {
    save_png(dir / "depth_start_raw.png", images.depth_start);
    save_png(dir / "depth_start.png", depth_picture(images.depth_start));
  }
  std::map<u32, std::string> texture_files;  // by handle
  for (const gx::TextureUpload& up : frame.uploads) {
    auto it = cap.textures.find(up.handle);
    if (it == cap.textures.end()) continue;
    video::Image image;
    image.width = up.width;
    image.height = up.height;
    const u8* texels = frame.texels.data() + up.offset;
    image.rgba.assign(texels, texels + std::size_t{up.width} * up.height * 4);
    const std::string name = texture_file(up.handle, it->second);
    if (save_png(dir / "textures" / name, image)) texture_files[up.handle] = "textures/" + name;
  }
  std::map<u32, std::string> copy_files;  // by command
  std::map<u32, const gx::FrameCapture::Copy*> copies_by_command;
  for (const gx::FrameCapture::Copy& c : cap.copies) copies_by_command[c.command] = &c;
  for (const video::CaptureImages::Copy& c : images.copies) {
    const auto it = copies_by_command.find(c.command);
    const gx::CopyCmd& cmd = frame.copies[frame.commands[c.command].index];
    const std::string name = format("c%u_%08X_%s_%ux%u.png", c.command, it != copies_by_command.end() ? it->second->address : 0,
                                    gx::copy_format_name(cmd.format), cmd.width, cmd.height);
    if (save_png(dir / "copies" / name, c.image)) copy_files[c.command] = "copies/" + name;
  }
  for (const video::CaptureImages::Earlier& e : images.earlier) {
    fs::create_directories(dir / "earlier", ec);
    auto it = cap.textures.find(e.handle);
    const std::string name = format("e%u_%08X_%ux%u.png", e.handle, it != cap.textures.end() ? it->second.address : 0,
                                    e.image.width, e.image.height);
    if (save_png(dir / "earlier" / name, e.image)) texture_files[e.handle] = "earlier/" + name;
  }

  // Draws by command, and who drew what.
  std::map<s32, const gx::FrameCapture::Draw*> draws_by_index;  // DrawCmd index
  std::vector<const gx::FrameCapture::Draw*> undrawn;
  for (const gx::FrameCapture::Draw& d : cap.draws) {
    if (d.draw >= 0) draws_by_index[d.draw] = &d;
    else undrawn.push_back(&d);
  }
  struct ByFunction {
    u32 draws = 0, triangles = 0;
  };
  std::map<std::string, ByFunction> by_function, by_owner;

  const u32 efb_width = frame.efb_width;
  std::string text;
  const GameStatus& s = info.status;
  text += format("OpenGP frame capture: %s, frame %llu, scene %u (%s%s%s%s%s)\n", info.game.c_str(),
                 static_cast<unsigned long long>(info.number), s.scene_id, s.activity.empty() ? "" : s.activity.c_str(),
                 s.activity.empty() || s.course.empty() ? "" : ", ", s.course.c_str(),
                 s.character.empty() ? "" : ", ", s.character.c_str());
  text += format("EFB %u x %u game pixels (widescreen: %u added each side), internal resolution %ux; XFB %u x %u; %s\n",
                 efb_width, gx::kEfbHeight, cap.side, info.scale, frame.xfb_width, frame.xfb_height, info.settings.c_str());
  text += format("%zu commands: %zu draws (%zu primitives drew nothing), %zu clears, %zu copies; %zu textures decoded\n",
                 frame.commands.size(), frame.draws.size(), undrawn.size(), frame.clears.size(), frame.copies.size(),
                 frame.uploads.size());
  if (cap.partials)
    text += format("Note: %u partial frame(s) (EFB readbacks) came first; only the draws after the last are here.\n", cap.partials);
  text +=
      "Coordinates are game pixels (0-640 x 0-528; widescreen draws reach past 0 and 640). Depth is the EFB's "
      "(0 near, 1 far).\nFiles: screen.png, efb_end.png, depth_end.png (near white), depth_end_raw.png (24-bit), "
      "efb_start.png, textures/, copies/, earlier/.\n\n";

  JsonArray commands;
  std::string body;
  for (std::size_t i = 0; i < frame.commands.size(); ++i) {
    const gx::Command& c = frame.commands[i];
    switch (c.type) {
      case gx::Command::Type::Clear: {
        const gx::ClearCmd& cl = frame.clears[c.index];
        body += format("#%zu clear %u,%u %ux%u (host EFB pixels)%s%s%s rgba %08X z %.6f\n", i, cl.x, cl.y, cl.width,
                       cl.height, cl.color ? " colour" : "", cl.alpha ? " alpha" : "", cl.depth ? " depth" : "", cl.rgba, cl.z);
        commands.add(Json()
                         .add("i", i)
                         .add("type", "clear")
                         .add("rect", format("%u,%u,%u,%u", cl.x, cl.y, cl.width, cl.height))
                         .add("color", cl.color)
                         .add("alpha", cl.alpha)
                         .add("depth", cl.depth)
                         .add("rgba", hex8(cl.rgba))
                         .add("z", cl.z));
        break;
      }
      case gx::Command::Type::Copy: {
        const gx::CopyCmd& cp = frame.copies[c.index];
        const auto it = copies_by_command.find(static_cast<u32>(i));
        const gx::FrameCapture::Copy* cc = it != copies_by_command.end() ? it->second : nullptr;
        const std::string file = copy_files.count(static_cast<u32>(i)) ? copy_files[static_cast<u32>(i)] : "";
        std::vector<std::string> by;
        if (cc) {
          by = host_guest_functions(cc->stacks, 3, gx_library);
          if (by.empty()) by = guest_functions(names, cc->stacks);
        }
        body += format("#%zu copy %s: EFB %u,%u %ux%u -> %s %s %ux%u%s%s%s%s\n", i,
                       cp.format == gx::CopyFormat::XFB ? "to the XFB (the frame shown)" : "to a texture",
                       cc ? cc->x : cp.src_x, cc ? cc->y : cp.src_y, cc ? cc->width : cp.src_width,
                       cc ? cc->height : cp.src_height, cc ? hex8(cc->address).c_str() : "?",
                       gx::copy_format_name(cp.format), cp.width, cp.height, cp.depth ? " (depth)" : "",
                       cc && gx::copy_clear(cc->trigger) ? " + clear" : "",
                       file.empty() ? "" : (" -> " + file).c_str(), "");
        if (!by.empty()) body += "     by " + join(by, " <- ") + "\n";
        Json j;
        j.add("i", i).add("type", "copy").add("to_xfb", cp.format == gx::CopyFormat::XFB);
        if (cc) {
          j.add("address", hex8(cc->address))
              .add("rect", format("%u,%u,%u,%u", cc->x, cc->y, cc->width, cc->height))
              .add("clear", gx::copy_clear(cc->trigger))
              .add("efb_format", gx::pixel_format_name(cc->pixel_format));
        }
        j.add("format", gx::copy_format_name(cp.format)).add("width", cp.width).add("height", cp.height).add("depth", cp.depth);
        if (!file.empty()) j.add("file", file);
        j.raw("by", json_strings(by).str());
        commands.add(j);
        break;
      }
      case gx::Command::Type::Readback:
        body += format("#%zu EFB readback (for a CPU read)\n", i);
        commands.add(Json().add("i", i).add("type", "readback"));
        break;
      case gx::Command::Type::Draw: {
        const gx::DrawCmd& dc = frame.draws[c.index];
        const gx::DrawState& st = frame.states[dc.state];
        const gx::PixelUniforms& u = frame.uniforms[st.uniforms];
        const auto it = draws_by_index.find(static_cast<s32>(c.index));
        const gx::FrameCapture::Draw* d = it != draws_by_index.end() ? it->second : nullptr;
        std::vector<std::string> host_by, guest_by;
        std::string library;
        if (d) {
          host_by = host_guest_functions(d->stacks, 12, gx_library, &library);
          guest_by = guest_functions(names, d->stacks);
        }
        const std::string who = !host_by.empty() ? host_by[0] : !guest_by.empty() ? guest_by[0] : "?";
        ByFunction& bf = by_function[who];
        ++bf.draws;
        bf.triangles += dc.index_count / 3;
        const std::string own = owner(host_by);
        ByFunction& bo = by_owner[own];
        ++bo.draws;
        bo.triangles += dc.index_count / 3;
        if (host_by.size() > 5) host_by.resize(5);
        static constexpr const char* kCull[4] = {"none", "back", "front", "all"};
        static constexpr const char* kWide[4] = {"", ", widened (2D background)", ", edges extended", ", widened overlay"};
        if (d) {
          body += format("#%zu draw %u: %s %u verts -> %u tris, %s, x %.1f..%.1f y %.1f..%.1f z %.6f..%.6f w %.3g..%.3g%s\n",
                         i, c.index, gx::primitive_name(d->primitive), d->count, dc.index_count / 3,
                         d->perspective ? "3D" : "2D", d->x0, d->x1, d->y0, d->y1, d->z0, d->z1, d->w0, d->w1,
                         kWide[std::min(d->widescreen, 3u)]);
        } else {
          body += format("#%zu draw %u: %u tris\n", i, c.index, dc.index_count / 3);
        }
        if (!host_by.empty())
          body += "     by " + join(host_by, " <- ") + (library.empty() ? "" : " (decoded in " + library + ")");
        if (!guest_by.empty()) body += (host_by.empty() ? "     by " : " | guest lr ") + join(guest_by, " <- ");
        if (!host_by.empty() || !guest_by.empty()) body += "\n";
        body += format("     state %u: %s; cull %s; scissor %d,%d %dx%d\n", dc.state,
                       gx::describe_pipeline(st.pipeline.bits).c_str(), kCull[gx::genmode_cull_mode(u.misc[0])],
                       st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]);
        body += format("     tev (%u stages):\n", gx::genmode_num_tev_stages(u.misc[0])) + gx::describe_tev(u);
        JsonArray textures;
        for (u32 m = 0; m < 8; ++m) {
          const gx::TextureBinding& tb = st.textures[m];
          if (!tb.texture) continue;
          const auto t = cap.textures.find(tb.texture);
          const std::string file = texture_files.count(tb.texture) ? texture_files[tb.texture] : "";
          Json tj;
          tj.add("map", m).add("handle", tb.texture).add("sampler", gx::describe_sampler(tb.mode0, tb.mode1));
          if (t != cap.textures.end()) {
            const gx::FrameCapture::Texture& tx = t->second;
            body += format("     tex%u: #%u %s%08X %s %ux%u%s%s | %s%s\n", m, tb.texture, tx.tmem ? "TMEM " : "", tx.address,
                           gx::texture_format_name(tx.format), tx.width, tx.height,
                           tx.levels > 1 ? format(" (%u levels)", tx.levels).c_str() : "",
                           tx.copy ? format(" EFB copy (as %s)", gx::texture_format_name(tx.copy_format)).c_str() : "",
                           gx::describe_sampler(tb.mode0, tb.mode1).c_str(), file.empty() ? "" : (" -> " + file).c_str());
            tj.add("address", hex8(tx.address))
                .add("format", gx::texture_format_name(tx.format))
                .add("width", tx.width)
                .add("height", tx.height)
                .add("levels", tx.levels)
                .add("tmem", tx.tmem)
                .add("efb_copy", tx.copy);
          } else {
            body += format("     tex%u: #%u | %s\n", m, tb.texture, gx::describe_sampler(tb.mode0, tb.mode1).c_str());
          }
          if (!file.empty()) tj.add("file", file);
          textures.add(tj);
        }
        if (d) {
          body += "     vertices: " + vertex_format(d->vcd_lo, d->vcd_hi) +
                  format("; xf: %u colours, %u texgens; col0 %s", d->xf_colors, d->xf_texgens, channel(d->color_ctrl[0]).c_str()) +
                  "\n";
        }
        Json j;
        j.add("i", i).add("type", "draw").add("draw", c.index).add("state", dc.state).add("triangles", dc.index_count / 3);
        if (d) {
          j.add("primitive", gx::primitive_name(d->primitive))
              .add("count", d->count)
              .add("perspective", d->perspective)
              .raw("bounds", format("[%.2f,%.2f,%.2f,%.2f]", d->x0, d->y0, d->x1, d->y1))
              .raw("depth", format("[%.7f,%.7f]", d->z0, d->z1))
              .raw("w", format("[%.4g,%.4g]", d->w0, d->w1))
              .add("widescreen", d->widescreen)
              .add("vertices", vertex_format(d->vcd_lo, d->vcd_hi))
              .add("genmode", hex8(d->genmode))
              .add("zmode", hex8(d->zmode))
              .add("blendmode", hex8(d->blendmode))
              .add("color0", channel(d->color_ctrl[0]));
        }
        j.add("pipeline", gx::describe_pipeline(st.pipeline.bits))
            .add("cull", kCull[gx::genmode_cull_mode(u.misc[0])])
            .raw("scissor", format("[%d,%d,%d,%d]", st.scissor[0], st.scissor[1], st.scissor[2], st.scissor[3]))
            .add("tev", gx::describe_tev(u))
            .raw("textures", textures.str())
            .raw("by", json_strings(host_by).str())
            .raw("guest_lr", json_strings(guest_by).str());
        j.add("owner", own);
        commands.add(j);
        break;
      }
    }
  }
  if (!undrawn.empty()) {
    body += format("\n%zu primitives drew nothing (culled, or clipped by a zero scissor):\n", undrawn.size());
    for (const gx::FrameCapture::Draw* d : undrawn) {
      std::vector<std::string> by = host_guest_functions(d->stacks, 2, gx_library);
      if (by.empty()) by = guest_functions(names, d->stacks);
      body += format("  %s %u verts, %s, x %.1f..%.1f y %.1f..%.1f by %s\n", gx::primitive_name(d->primitive), d->count,
                     d->perspective ? "3D" : "2D", d->x0, d->x1, d->y0, d->y1, join(by, " <- ").c_str());
    }
  }

  JsonArray functions, owners;
  auto table = [&](const std::map<std::string, ByFunction>& by, const char* title, JsonArray& out) {
    std::vector<std::pair<std::string, ByFunction>> sorted(by.begin(), by.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.triangles > b.second.triangles; });
    text += title;
    for (const auto& [name, bf] : sorted) {
      text += format("  %5u %8u  %s\n", bf.draws, bf.triangles, name.c_str());
      out.add(Json().add("function", name).add("draws", bf.draws).add("triangles", bf.triangles));
    }
  };
  table(by_function, "Draws by the game function that drew them (draws, triangles):\n", functions);
  table(by_owner, "... by the code that owns them, outside the HSD engine (draws, triangles):\n", owners);
  text += "\n" + body;

  std::ofstream(dir / "capture.txt", std::ios::binary) << text;
  Json j;
  j.add("format", 1)
      .add("game", info.game)
      .add("frame", info.number)
      .add("scene", s.scene_id)
      .add("activity", s.activity)
      .add("course", s.course)
      .add("efb_width", efb_width)
      .add("efb_height", gx::kEfbHeight)
      .add("side", cap.side)
      .add("scale", info.scale)
      .add("xfb_width", frame.xfb_width)
      .add("xfb_height", frame.xfb_height)
      .add("partials", cap.partials)
      .raw("functions", functions.str())
      .raw("owners", owners.str())
      .raw("commands", commands.str());
  std::ofstream(dir / "capture.json", std::ios::binary) << j.str() << "\n";
  return true;
}

}  // namespace app
