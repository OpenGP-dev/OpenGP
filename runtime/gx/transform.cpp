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

// Material times light, with the hardware's rounding.
u8 modulate(u8 material, float light) {
  const int l = std::clamp(static_cast<int>(light), 0, 255);
  return static_cast<u8>((material * (l + (l >> 7))) >> 8);
}

}  // namespace

XfUnit::XfUnit() {
  // One colour channel until the game says otherwise.
  write(XF_NUM_COLORS, 1);
}

void XfUnit::prepare() {
  if (!dirty_) return;
  dirty_ = false;

  perspective_ = mem_[XF_PROJECTION + 6] == 0;
  for (u32 i = 0; i < 6; ++i) proj_[i] = fmem_[XF_PROJECTION + i];
  num_colors_ = mem_[XF_NUM_COLORS] & 3;

  auto channel = [](u32 v) {
    const bool lighting = bits(v, 1, 1);
    return Channel{static_cast<bool>(bits(v, 0, 1)), lighting, static_cast<bool>(bits(v, 6, 1)), bits(v, 7, 2),
                   bits(v, 9, 2), lighting ? (bits(v, 2, 4) | (bits(v, 11, 4) << 4)) : 0u};
  };
  auto rgba = [](u32 v, u8* out) {
    out[0] = static_cast<u8>(v >> 24), out[1] = static_cast<u8>(v >> 16), out[2] = static_cast<u8>(v >> 8),
    out[3] = static_cast<u8>(v);
  };
  lighting_ = false;
  for (u32 c = 0; c < 2; ++c) {
    color_[c] = channel(mem_[XF_COLOR0_CTRL + c]);
    alpha_[c] = channel(mem_[XF_ALPHA0_CTRL + c]);
    rgba(mem_[XF_MATERIAL0 + c], material_[c]);
    rgba(mem_[XF_AMBIENT0 + c], ambient_[c]);
    lighting_ = lighting_ || color_[c].lighting || alpha_[c].lighting;
  }

  // Lights: word 3 colour (RGBA8), 4-6 cosine attenuation, 7-9 distance
  // attenuation, 10-12 position, 13-15 direction (or half angle).
  for (u32 i = 0; i < 8; ++i) {
    const u32 base = XF_LIGHTS + 16 * i;
    Light& l = lights_[i];
    const u32 color = mem_[base + 3];
    for (u32 k = 0; k < 4; ++k) l.color[k] = static_cast<float>((color >> (24 - 8 * k)) & 0xFF);
    for (u32 k = 0; k < 3; ++k) {
      l.cosatt[k] = fmem_[base + 4 + k];
      l.distatt[k] = fmem_[base + 7 + k];
      l.pos[k] = fmem_[base + 10 + k];
      l.dir[k] = fmem_[base + 13 + k];
    }
  }

  num_texgens_ = std::min<u32>(mem_[XF_NUM_TEXGENS] & 0xF, 8);
  dual_tex_ = mem_[XF_DUAL_TEX] & 1;
  normals_ = lighting_;
  for (u32 i = 0; i < num_texgens_; ++i) {
    const u32 v = mem_[XF_TEXGEN + i], post = mem_[XF_POST_TEXGEN + i];
    TexGen& t = texgens_[i];
    t = {static_cast<bool>(bits(v, 1, 1)), static_cast<bool>(bits(v, 2, 1)), bits(v, 4, 3), bits(v, 7, 5),
         bits(v, 12, 3), bits(v, 15, 3), bits(post, 0, 6), static_cast<bool>(bits(post, 8, 1))};
    if (t.type == 1 || (t.type == 0 && (t.source == 1 || t.source == 3 || t.source == 4))) normals_ = true;
  }
}

void XfUnit::transform_lit(const InputVertex& in, TransformedVertex& out) const {
  // Position: model-view matrix (3x4), then projection.
  const Vec3 view = mul34(&fmem_[XF_POS_MATRICES + in.posmtx * 4], vec(in.pos), true);
  const float* p = proj_;
  if (perspective_) {
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
  Vec3 normal[3];
  if (normals_) {
    const float* nm = &fmem_[XF_NORMAL_MATRICES + (in.posmtx & 31) * 3];
    normal[0] = mul33(nm, vec(in.normal[0])).normalized();
    normal[1] = mul33(nm, vec(in.normal[1]));
    normal[2] = mul33(nm, vec(in.normal[2]));
  }

  // ---- Colour channels ----
  // Attenuation of one light; updates `ldir` to the normalised light direction.
  auto attenuation = [&](const Light& l, Vec3& ldir, const Channel& ch) {
    float attn = 1.0f;
    switch (ch.attenuation) {
      case 0:  // none
      case 2:  // directional
        ldir = ldir.normalized();
        if (ldir.is_zero()) ldir = normal[0];
        break;
      case 1: {  // specular
        ldir = ldir.normalized();
        attn = ldir.dot(normal[0]) >= 0.0f ? std::max(0.0f, vec(l.dir).dot(normal[0])) : 0.0f;
        const Vec3 att{1.0f, attn, attn * attn};
        Vec3 dist = vec(l.distatt);
        if (ch.diffuse != 0) dist = dist.normalized();
        attn = safe_divide(std::max(0.0f, att.dot(vec(l.cosatt))), att.dot(dist));
        break;
      }
      default: {  // spot
        const float dist2 = ldir.length_sq();
        const float dist = std::sqrt(dist2);
        ldir = ldir / dist;
        attn = std::max(0.0f, ldir.dot(vec(l.dir)));
        const float cos_att = l.cosatt[0] + l.cosatt[1] * attn + l.cosatt[2] * attn * attn;
        const float dist_att = l.distatt[0] + l.distatt[1] * dist + l.distatt[2] * dist2;
        attn = safe_divide(std::max(0.0f, cos_att), dist_att);
        break;
      }
    }
    return attn;
  };
  // Scale applied to a light's colour for this vertex (diffuse term included).
  auto light_scale = [&](const Light& l, const Channel& ch) {
    Vec3 ldir = vec(l.pos) - view;
    const float attn = attenuation(l, ldir, ch);
    const float dif = ldir.dot(normal[0]);
    switch (ch.diffuse) {
      case 0: return attn;
      case 1: return attn * dif;
      default: return attn * std::max(0.0f, dif);
    }
  };

  u8 colors[2][4];
  for (u32 c = 0; c < 2; ++c) {
    u8* result = colors[c];
    const Channel& color = color_[c];
    const u8* mat = color.material_from_vertex ? in.color[c].data() : material_[c];
    if (color.lighting) {
      const u8* amb = color.ambient_from_vertex ? in.color[c].data() : ambient_[c];
      float lit[3] = {static_cast<float>(amb[0]), static_cast<float>(amb[1]), static_cast<float>(amb[2])};
      for (u32 mask = color.light_mask; mask; mask &= mask - 1) {
        const Light& l = lights_[std::countr_zero(mask)];
        const float s = light_scale(l, color);
        for (u32 k = 0; k < 3; ++k) lit[k] += l.color[k] * s;
      }
      for (u32 k = 0; k < 3; ++k) result[k] = modulate(mat[k], lit[k]);
    } else {
      for (u32 k = 0; k < 3; ++k) result[k] = mat[k];
    }

    const Channel& alpha = alpha_[c];
    const u8 mat_a = alpha.material_from_vertex ? in.color[c][3] : material_[c][3];
    if (alpha.lighting) {
      float lit = alpha.ambient_from_vertex ? in.color[c][3] : ambient_[c][3];
      for (u32 mask = alpha.light_mask; mask; mask &= mask - 1) {
        const Light& l = lights_[std::countr_zero(mask)];
        lit += l.color[3] * light_scale(l, alpha);
      }
      result[3] = modulate(mat_a, lit);
    } else {
      result[3] = mat_a;
    }
  }

  // ---- Texture coordinates ----
  for (u32 i = 0; i < num_texgens_; ++i) {
    const TexGen& tg = texgens_[i];
    if (tg.type != 1) {
      texgen(i, in, colors, out);
      continue;
    }
    // Emboss: offset an earlier coordinate by the light direction.
    const Light& l = lights_[tg.emboss_light];
    const Vec3 ldir = (vec(l.pos) - view).normalized();
    const float* src = out.tex[tg.emboss_source];
    const Vec3 dst = {src[0] + ldir.dot(normal[1]), src[1] + ldir.dot(normal[2]), src[2]};
    out.tex[i][0] = dst.x, out.tex[i][1] = dst.y, out.tex[i][2] = dst.z;
  }

  // The number of colour channels controls what the TEV sees (colour
  // texgens above still use both).
  auto pack = [](const u8* c) { return c[0] | (u32{c[1]} << 8) | (u32{c[2]} << 16) | (u32{c[3]} << 24); };
  out.color[0] = num_colors_ >= 1 ? pack(colors[0]) : 0;
  out.color[1] = num_colors_ >= 2 ? pack(colors[1]) : 0;
}

}  // namespace gx
