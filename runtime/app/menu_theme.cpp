// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/menu_theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// The typeface (thirdparty/fonts/mplus-rounded-1c), embedded by cmake/embed.cmake.
extern const unsigned char kMenuFontRegular[];
extern const std::size_t kMenuFontRegular_size;
extern const unsigned char kMenuFontExtraBold[];
extern const std::size_t kMenuFontExtraBold_size;

namespace app::theme {

namespace {

float g_scale = 1.0f;
ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;

// A stroke with round caps and joins (ImGui's polylines have square ends).
void stroke(ImDrawList* dl, const ImVec2* points, int count, ImU32 colour, float thickness, bool closed = false) {
  dl->AddPolyline(points, count, colour, thickness, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
  for (int i = 0; i < count; ++i) dl->AddCircleFilled(points[i], thickness * 0.5f, colour);
}

}  // namespace

ImU32 alpha(ImU32 colour, float a) {
  const float own = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
  const auto v = static_cast<ImU32>(std::lround(std::clamp(own * a, 0.0f, 1.0f) * 255.0f));
  return (colour & ~IM_COL32_A_MASK) | (v << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
  const ImVec4 va = ImGui::ColorConvertU32ToFloat4(a), vb = ImGui::ColorConvertU32ToFloat4(b);
  t = std::clamp(t, 0.0f, 1.0f);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(va.x + (vb.x - va.x) * t, va.y + (vb.y - va.y) * t,
                                               va.z + (vb.z - va.z) * t, va.w + (vb.w - va.w) * t));
}

void set_scale(float scale) { g_scale = scale; }
float scale() { return g_scale; }
float px(float units) { return std::round(units * g_scale); }

void push_font(Weight weight, float size) { ImGui::PushFont(weight == Weight::Bold ? g_bold : g_regular, size); }
void pop_font() { ImGui::PopFont(); }

void load_fonts(ImGuiIO& io) {
  ImFontConfig cfg;
  cfg.FontDataOwnedByAtlas = false;  // the arrays live for the whole run
  g_regular = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFontRegular),
                                             static_cast<int>(kMenuFontRegular_size), 0.0f, &cfg);
  g_bold = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kMenuFontExtraBold),
                                          static_cast<int>(kMenuFontExtraBold_size), 0.0f, &cfg);
  io.FontDefault = g_regular;
}

void apply_style(ImGuiStyle& style, float scale) {
  style = ImGuiStyle();
  style.WindowPadding = ImVec2(0, 0);
  style.WindowRounding = 16;
  style.WindowBorderSize = 0;
  style.ChildRounding = 0;
  style.ChildBorderSize = 0;
  style.PopupRounding = 8;
  style.PopupBorderSize = 1;
  style.FramePadding = ImVec2(12, 8);
  style.FrameRounding = 8;
  style.FrameBorderSize = 0;
  style.ItemSpacing = ImVec2(10, 10);
  style.ItemInnerSpacing = ImVec2(8, 6);
  style.ScrollbarSize = 10;
  style.ScrollbarRounding = 5;
  style.GrabMinSize = 24;
  style.GrabRounding = 5;
  style.DisabledAlpha = 0.45f;
  style.WindowMinSize = ImVec2(1, 1);

  ImVec4* c = style.Colors;
  auto col = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
  const ImVec4 none(0, 0, 0, 0);
  c[ImGuiCol_Text] = col(kChalk);
  c[ImGuiCol_TextDisabled] = col(kDim);
  c[ImGuiCol_WindowBg] = col(kAsphalt);
  c[ImGuiCol_ChildBg] = none;
  c[ImGuiCol_PopupBg] = col(kPanel);
  c[ImGuiCol_Border] = col(kLine);
  c[ImGuiCol_BorderShadow] = none;
  c[ImGuiCol_FrameBg] = col(kPanel);
  c[ImGuiCol_FrameBgHovered] = col(kPanelHover);
  c[ImGuiCol_FrameBgActive] = col(kPanelHover);
  c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col(kAsphaltDeep);
  c[ImGuiCol_ScrollbarBg] = none;
  c[ImGuiCol_ScrollbarGrab] = col(kLine);
  c[ImGuiCol_ScrollbarGrabHovered] = col(kFaint);
  c[ImGuiCol_ScrollbarGrabActive] = col(kDim);
  c[ImGuiCol_Button] = col(kPanel);
  c[ImGuiCol_ButtonHovered] = c[ImGuiCol_ButtonActive] = col(kPanelHover);
  c[ImGuiCol_Header] = col(kPanelHover);
  c[ImGuiCol_HeaderHovered] = c[ImGuiCol_HeaderActive] = col(kPanelHover);
  c[ImGuiCol_Separator] = col(kLine);
  c[ImGuiCol_NavCursor] = none;  // the widgets draw their own focus rings
  c[ImGuiCol_NavWindowingHighlight] = none;
  c[ImGuiCol_NavWindowingDimBg] = none;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);  // behind a confirmation
  style.ScaleAllSizes(scale);
  style.FontSizeBase = kBody;
  style.FontScaleMain = 1.0f;
  style.FontScaleDpi = scale;
}

void draw_icon(ImDrawList* dl, Icon icon, ImVec2 centre, float size, ImU32 colour) {
  const float w = size, h = size;
  const ImVec2 o(centre.x - w * 0.5f, centre.y - h * 0.5f);
  auto at = [&](float fx, float fy) { return ImVec2(o.x + w * fx, o.y + h * fy); };
  const float t = std::max(2.0f * g_scale, w / 9.0f);
  switch (icon) {
    case Icon::Plus: {
      const ImVec2 v[] = {at(0.5f, 0.15f), at(0.5f, 0.85f)}, hz[] = {at(0.15f, 0.5f), at(0.85f, 0.5f)};
      stroke(dl, v, 2, colour, t);
      stroke(dl, hz, 2, colour, t);
      break;
    }
    case Icon::Cross: {
      const ImVec2 a[] = {at(0.22f, 0.22f), at(0.78f, 0.78f)}, b[] = {at(0.78f, 0.22f), at(0.22f, 0.78f)};
      stroke(dl, a, 2, colour, t);
      stroke(dl, b, 2, colour, t);
      break;
    }
    case Icon::ChevronDown: {
      const ImVec2 p[] = {at(0.22f, 0.38f), at(0.5f, 0.64f), at(0.78f, 0.38f)};
      stroke(dl, p, 3, colour, t);
      break;
    }
    case Icon::ChevronLeft: {
      const ImVec2 p[] = {at(0.62f, 0.22f), at(0.36f, 0.5f), at(0.62f, 0.78f)};
      stroke(dl, p, 3, colour, t);
      break;
    }
    case Icon::ChevronRight: {
      const ImVec2 p[] = {at(0.38f, 0.22f), at(0.64f, 0.5f), at(0.38f, 0.78f)};
      stroke(dl, p, 3, colour, t);
      break;
    }
    case Icon::Check: {
      const ImVec2 p[] = {at(0.2f, 0.52f), at(0.42f, 0.74f), at(0.8f, 0.3f)};
      stroke(dl, p, 3, colour, t);
      break;
    }
    case Icon::Info: {
      dl->AddCircle(centre, w * 0.44f, colour, 0, t);
      const ImVec2 p[] = {at(0.5f, 0.47f), at(0.5f, 0.72f)};
      stroke(dl, p, 2, colour, t);
      dl->AddCircleFilled(at(0.5f, 0.3f), t * 0.6f, colour);
      break;
    }
    case Icon::Warning: {
      const ImVec2 tri[] = {at(0.5f, 0.12f), at(0.92f, 0.86f), at(0.08f, 0.86f)};
      stroke(dl, tri, 3, colour, t, true);
      const ImVec2 p[] = {at(0.5f, 0.4f), at(0.5f, 0.58f)};
      stroke(dl, p, 2, colour, t);
      dl->AddCircleFilled(at(0.5f, 0.72f), t * 0.6f, colour);
      break;
    }
    case Icon::Gear: {
      dl->AddCircle(centre, w * 0.26f, colour, 0, t);
      for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 4.0f;
        const ImVec2 p[] = {ImVec2(centre.x + std::cos(a) * w * 0.3f, centre.y + std::sin(a) * w * 0.3f),
                            ImVec2(centre.x + std::cos(a) * w * 0.46f, centre.y + std::sin(a) * w * 0.46f)};
        stroke(dl, p, 2, colour, t);
      }
      break;
    }
  }
}

}  // namespace app::theme
