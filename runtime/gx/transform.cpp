// SPDX-License-Identifier: GPL-3.0-or-later

#include "gx/transform.h"

#include <algorithm>
#include <cmath>

namespace gx {

namespace {

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  float length_sq() const { return dot(*this); }
  Vec3 normalized() const {
    const float len = std::sqrt(length_sq());
    return len != 0.0f ? *this / len : *this;
  }
  bool is_zero() const { return x == 0.0f && y == 0.0f && z == 0.0f; }
};

Vec3 vec(const float* v) { return {v[0], v[1], v[2]}; }

// 3x4 (or 2x4) matrix rows times (x, y, z, 1).
Vec3 mul34(const float* m, const Vec3& v, bool rows3) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
          rows3 ? m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11] : 1.0f};
}

Vec3 mul33(const float* m, const Vec3& v) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
          m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

float safe_divide(float n, float d) { return d == 0.0f ? (n > 0.0f ? 1.0f : 0.0f) : n / d; }

// Colour channel control (XF_COLOR0_CTRL etc.)
struct Channel {
  u32 v;
  bool material_from_vertex() const { return bits(v, 0, 1); }
  bool lighting() const { return bits(v, 1, 1); }
  bool ambient_from_vertex() const { return bits(v, 6, 1); }
  u32 diffuse() const { return bits(v, 7, 2); }  // 0 none, 1 sign, 2 clamp
  u32 attenuation() const { return bits(v, 9, 2); }  // 0 none, 1 spec, 2 dir, 3 spot
  u32 light_mask() const { return lighting() ? (bits(v, 2, 4) | (bits(v, 11, 4) << 4)) : 0; }
};

// Texture coordinate generation control (XF_TEXGEN + n).
struct TexGen {
  u32 v;
  bool stq() const { return bits(v, 1, 1); }
  bool abc1() const { return bits(v, 2, 1); }  // else AB11
  u32 type() const { return bits(v, 4, 3); }   // 0 regular, 1 emboss, 2 colour0, 3 colour1
  u32 source() const { return bits(v, 7, 5); }  // 0 position, 1 normal, 2 colours, 3 tangent, 4 binormal, 5+ tex n
  u32 emboss_source() const { return bits(v, 12, 3); }
  u32 emboss_light() const { return bits(v, 15, 3); }
};

}  // namespace

XfUnit::XfUnit() {
  // Sensible reset values: identity-ish viewport and one colour channel.
  mem_[XF_NUM_COLORS] = 1;
}

void XfUnit::transform(const InputVertex& in, TransformedVertex& out) const {
  // Position: model-view matrix (3x4), then projection.
  std::array<float, 12> m;
  for (u32 i = 0; i < 12; ++i) m[i] = f(static_cast<u16>(XF_POS_MATRICES + in.posmtx * 4 + i));
  const Vec3 view = mul34(m.data(), vec(in.pos), true);
  out.view[0] = view.x, out.view[1] = view.y, out.view[2] = view.z;

  float p[6];
  for (u32 i = 0; i < 6; ++i) p[i] = f(static_cast<u16>(XF_PROJECTION + i));
  if (reg(XF_PROJECTION + 6) == 0) {  // perspective
    out.clip[0] = p[0] * view.x + p[1] * view.z;
    out.clip[1] = p[2] * view.y + p[3] * view.z;
    out.clip[2] = (p[4] * view.z + p[5]) * (1.0f - 1e-7f);
    out.clip[3] = -view.z;
  } else {
    out.clip[0] = p[0] * view.x + p[1];
    out.clip[1] = p[2] * view.y + p[3];
    out.clip[2] = p[4] * view.z + p[5];
    out.clip[3] = 1.0f;
  }

  // Normals: 3x3 normal matrix. Only the normal is renormalised: the scale
  // of the tangent and binormal controls emboss mapping.
  std::array<float, 9> n;
  for (u32 i = 0; i < 9; ++i) n[i] = f(static_cast<u16>(XF_NORMAL_MATRICES + (in.posmtx & 31) * 3 + i));
  for (u32 k = 0; k < 3; ++k) {
    Vec3 r = mul33(n.data(), vec(in.normal[k]));
    if (k == 0) r = r.normalized();
    out.normal[k][0] = r.x, out.normal[k][1] = r.y, out.normal[k][2] = r.z;
  }

  transform_color(in, out);
  transform_texcoords(in, out);

  // The number of colour channels controls what the TEV sees (colour
  // texgens above still use both).
  const u32 num_colors = reg(XF_NUM_COLORS) & 3;
  if (num_colors == 0) out.color[0] = {0, 0, 0, 0};
  if (num_colors <= 1) out.color[1] = {0, 0, 0, 0};
}

void XfUnit::transform_color(const InputVertex& in, TransformedVertex& out) const {
  const Vec3 pos = vec(out.view);
  const Vec3 normal = vec(out.normal[0]);

  // Light layout: word 3 colour (RGBA8), 4-6 cosine attenuation, 7-9
  // distance attenuation, 10-12 position, 13-15 direction (or half angle).
  struct Light {
    u32 color;
    Vec3 cosatt, distatt, pos, dir;
  };
  auto light = [&](u32 i) {
    const u16 base = static_cast<u16>(XF_LIGHTS + 16 * i);
    auto v3 = [&](u16 o) { return Vec3{f(base + o), f(static_cast<u16>(base + o + 1)), f(static_cast<u16>(base + o + 2))}; };
    return Light{reg(base + 3), v3(4), v3(7), v3(10), v3(13)};
  };

  // Attenuation of one light; updates `ldir` to the normalised light direction.
  auto attenuation = [&](const Light& l, Vec3& ldir, const Channel& ch) {
    float attn = 1.0f;
    switch (ch.attenuation()) {
      case 0:  // none
      case 2:  // directional
        ldir = ldir.normalized();
        if (ldir.is_zero()) ldir = normal;
        break;
      case 1: {  // specular
        ldir = ldir.normalized();
        attn = ldir.dot(normal) >= 0.0f ? std::max(0.0f, l.dir.dot(normal)) : 0.0f;
        const Vec3 att{1.0f, attn, attn * attn};
        Vec3 dist = l.distatt;
        if (ch.diffuse() != 0) dist = dist.normalized();
        attn = safe_divide(std::max(0.0f, att.dot(l.cosatt)), att.dot(dist));
        break;
      }
      case 3: {  // spot
        const float dist2 = ldir.length_sq();
        const float dist = std::sqrt(dist2);
        ldir = ldir / dist;
        attn = std::max(0.0f, ldir.dot(l.dir));
        const float cos_att = l.cosatt.x + l.cosatt.y * attn + l.cosatt.z * attn * attn;
        const float dist_att = l.distatt.x + l.distatt.y * dist + l.distatt.z * dist2;
        attn = safe_divide(std::max(0.0f, cos_att), dist_att);
        break;
      }
    }
    return attn;
  };
  // Scale applied to a light's colour for this vertex (diffuse term included).
  auto light_scale = [&](const Light& l, const Channel& ch) {
    Vec3 ldir = l.pos - pos;
    const float attn = attenuation(l, ldir, ch);
    float dif = ldir.dot(normal);
    switch (ch.diffuse()) {
      case 0: return attn;
      case 1: return attn * dif;
      default: return attn * std::max(0.0f, dif);
    }
  };
  auto rgba = [](u32 v) {
    return std::array<u8, 4>{static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
  };
  // Material times light, with the hardware's rounding.
  auto modulate = [](u8 material, float light) {
    const int l = std::clamp(static_cast<int>(light), 0, 255);
    return static_cast<u8>((material * (l + (l >> 7))) >> 8);
  };

  for (u32 c = 0; c < 2; ++c) {
    const std::array<u8, 4> mat_reg = rgba(reg(static_cast<u16>(XF_MATERIAL0 + c)));
    const std::array<u8, 4> amb_reg = rgba(reg(static_cast<u16>(XF_AMBIENT0 + c)));
    std::array<u8, 4> result;

    const Channel color{reg(static_cast<u16>(XF_COLOR0_CTRL + c))};
    const std::array<u8, 4>& mat = color.material_from_vertex() ? in.color[c] : mat_reg;
    if (color.lighting()) {
      const std::array<u8, 4>& amb = color.ambient_from_vertex() ? in.color[c] : amb_reg;
      float lit[3] = {static_cast<float>(amb[0]), static_cast<float>(amb[1]), static_cast<float>(amb[2])};
      const u32 mask = color.light_mask();
      for (u32 i = 0; i < 8; ++i) {
        if (!(mask & (1u << i))) continue;
        const Light l = light(i);
        const float s = light_scale(l, color);
        const std::array<u8, 4> lc = rgba(l.color);
        for (u32 k = 0; k < 3; ++k) lit[k] += lc[k] * s;
      }
      for (u32 k = 0; k < 3; ++k) result[k] = modulate(mat[k], lit[k]);
    } else {
      for (u32 k = 0; k < 3; ++k) result[k] = mat[k];
    }

    const Channel alpha{reg(static_cast<u16>(XF_ALPHA0_CTRL + c))};
    const u8 mat_a = alpha.material_from_vertex() ? in.color[c][3] : mat_reg[3];
    if (alpha.lighting()) {
      float lit = alpha.ambient_from_vertex() ? in.color[c][3] : amb_reg[3];
      const u32 mask = alpha.light_mask();
      for (u32 i = 0; i < 8; ++i) {
        if (!(mask & (1u << i))) continue;
        const Light l = light(i);
        lit += (l.color & 0xFF) * light_scale(l, alpha);
      }
      result[3] = modulate(mat_a, lit);
    } else {
      result[3] = mat_a;
    }
    out.color[c] = result;
  }
}

void XfUnit::transform_texcoords(const InputVertex& in, TransformedVertex& out) const {
  const u32 count = std::min<u32>(num_texgens(), 8);
  const bool dual = reg(XF_DUAL_TEX) & 1;
  for (u32 i = 0; i < 8; ++i) out.tex[i][0] = out.tex[i][1] = 0.0f, out.tex[i][2] = 1.0f;

  for (u32 i = 0; i < count; ++i) {
    const TexGen tg{reg(static_cast<u16>(XF_TEXGEN + i))};
    Vec3 dst;
    switch (tg.type()) {
      case 0: {  // regular: matrix times a source row
        Vec3 src;
        switch (tg.source()) {
          case 0: src = vec(in.pos); break;
          case 1: src = vec(in.normal[0]); break;
          case 3: src = vec(in.normal[1]); break;
          case 4: src = vec(in.normal[2]); break;
          case 2: src = {}; break;  // colours: only valid for the colour texgen types
          default: {
            const u32 t = std::min<u32>(tg.source() - 5, 7);
            src = {in.tex[t][0], in.tex[t][1], 1.0f};
            break;
          }
        }
        if (std::isnan(src.x)) src.x = 1.0f;
        if (std::isnan(src.y)) src.y = 1.0f;
        if (std::isnan(src.z)) src.z = 1.0f;
        if (!tg.abc1()) src.z = 1.0f;

        std::array<float, 12> m;
        for (u32 k = 0; k < 12; ++k) m[k] = f(static_cast<u16>(XF_POS_MATRICES + in.texmtx[i] * 4 + k));
        dst = mul34(m.data(), src, tg.stq());

        if (dual) {
          const u32 post = reg(static_cast<u16>(XF_POST_TEXGEN + i));
          const Vec3 t = bits(post, 8, 1) ? dst.normalized() : dst;
          for (u32 k = 0; k < 12; ++k) m[k] = f(static_cast<u16>(XF_POST_MATRICES + bits(post, 0, 6) * 4 + k));
          dst = mul34(m.data(), t, true);
        }

        // q = 0 is a special case on hardware.
        if (dst.z == 0.0f) {
          dst.x = std::clamp(dst.x / 2.0f, -1.0f, 1.0f);
          dst.y = std::clamp(dst.y / 2.0f, -1.0f, 1.0f);
        }
        break;
      }
      case 1: {  // emboss: offset an earlier coordinate by the light direction
        const u16 base = static_cast<u16>(XF_LIGHTS + 16 * tg.emboss_light());
        const Vec3 lpos{f(base + 10), f(static_cast<u16>(base + 11)), f(static_cast<u16>(base + 12))};
        const Vec3 ldir = (lpos - vec(out.view)).normalized();
        const float* src = out.tex[tg.emboss_source()];
        dst = {src[0] + ldir.dot(vec(out.normal[1])), src[1] + ldir.dot(vec(out.normal[2])), src[2]};
        break;
      }
      default: {  // colour 0 / colour 1: red and green become s and t
        const auto& c = out.color[tg.type() - 2];
        dst = {c[0] / 255.0f, c[1] / 255.0f, 1.0f};
        break;
      }
    }
    out.tex[i][0] = dst.x, out.tex[i][1] = dst.y, out.tex[i][2] = dst.z;
  }
}

}  // namespace gx
