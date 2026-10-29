// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/menu_widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui_internal.h>

namespace app::widgets {

using theme::px;
using theme::Weight;

namespace {

constexpr float kPi = 3.14159265f;

// The typeface at a size, for the scope.
struct Font {
  Font(Weight weight, float size) { theme::push_font(weight, size); }
  ~Font() { theme::pop_font(); }
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
};

// A colour with the style's alpha (fades, disabled items) applied.
ImU32 col(ImU32 c) { return ImGui::GetColorU32(c); }

ImVec2 measure(const char* s, float wrap = -1.0f) { return ImGui::CalcTextSize(s, nullptr, false, wrap); }

void draw_text(ImDrawList* dl, ImVec2 pos, ImU32 c, const char* s, float wrap = 0.0f) {
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col(c), s, nullptr, wrap);
}

// Text centred in a box.
void draw_centred(ImDrawList* dl, const ImRect& bb, ImU32 c, const char* s) {
  const ImVec2 ts = measure(s);
  draw_text(dl, ImVec2(std::round(bb.Min.x + (bb.GetWidth() - ts.x) * 0.5f), std::round(bb.Min.y + (bb.GetHeight() - ts.y) * 0.5f)), c, s);
}

// A value that moves towards `target` over `seconds`, kept per item.
float animate(ImGuiID id, float target, float seconds) {
  float* v = ImGui::GetStateStorage()->GetFloatRef(id, target);
  const float step = ImGui::GetIO().DeltaTime / seconds;
  if (*v < target) *v = std::min(target, *v + step);
  else if (*v > target) *v = std::max(target, *v - step);
  return *v;
}

float ease(float t) {  // in-out cubic
  return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}

// An item's basics, shared by the buttons.
struct Item {
  ImGuiID id = 0;
  ImRect bb;
  bool ok = false;  // visible (not clipped)
  bool hovered = false, held = false, pressed = false;
};

LastItem g_last_item;
bool g_list_focus_pending = false;  // a drop-down list just opened

Item begin_item(const char* label, ImVec2 size, ImGuiButtonFlags flags = 0) {
  Item it;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return it;
  it.id = w->GetID(label);
  it.bb = ImRect(w->DC.CursorPos, w->DC.CursorPos + size);
  g_last_item = {it.id, it.bb.Min, it.bb.Max};
  ImGui::ItemSize(size);
  it.ok = ImGui::ItemAdd(it.bb, it.id);
  if (it.ok) it.pressed = ImGui::ButtonBehavior(it.bb, it.id, &it.hovered, &it.held, flags | ImGuiButtonFlags_NoHoveredOnFocus);
  return it;
}

// Claims the keys that change a focused row's value, so that navigation
// does not move on them, and tells whether one was pressed this frame.
struct Keys {
  bool left = false, right = false;
};

Keys row_keys(ImGuiID id) {
  Keys k;
  ImGuiContext& g = *GImGui;
  if (g.NavId != id || g.ActiveId != 0) return k;
  const bool pad = (g.IO.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
  static constexpr ImGuiKey kLeft[] = {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft};
  static constexpr ImGuiKey kRight[] = {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight};
  const ImGuiInputFlags repeat = static_cast<int>(ImGuiInputFlags_Repeat) | static_cast<int>(ImGuiInputFlags_RepeatRateNavTweak);
  for (int i = 0; i < 3; ++i) {
    ImGui::SetKeyOwner(kLeft[i], id);
    ImGui::SetKeyOwner(kRight[i], id);
    if (i > 0 && !pad) continue;
    k.left = k.left || ImGui::IsKeyPressed(kLeft[i], repeat, id);
    k.right = k.right || ImGui::IsKeyPressed(kRight[i], repeat, id);
  }
  return k;
}

ImU32 ring_colour(Kind kind) { return kind == Kind::Quiet ? theme::kBlue : theme::kChalk; }

// A toggle's track and knob, `on` moving over 120 ms.
void draw_toggle(ImDrawList* dl, ImGuiID id, ImVec2 min, bool on, bool focus) {
  const float w = px(42), h = px(24), r = h * 0.5f;
  const ImRect track(min, min + ImVec2(w, h));
  dl->AddRectFilled(track.Min, track.Max, col(on ? theme::kBlue : theme::kLine), r);
  if (focus) dl->AddRect(track.Min, track.Max, col(theme::kChalk), r, 0, px(2));
  const float t = ease(animate(id + 1, on ? 1.0f : 0.0f, 0.12f));
  const float knob = px(9), inset = px(3);
  const float x = track.Min.x + inset + knob + t * (w - 2 * (inset + knob));
  dl->AddCircleFilled(ImVec2(x, track.Min.y + r), knob, col(theme::kChalk));
}

// The chevron that marks a drop-down list.
void draw_choice_box(ImDrawList* dl, const ImRect& bb, const char* value, bool hovered, bool outline) {
  dl->AddRectFilled(bb.Min, bb.Max, col(hovered ? theme::kPanelHover : theme::kPanel), px(8));
  dl->AddRect(bb.Min, bb.Max, col(outline ? theme::kBlue : theme::kLine), px(8), 0, 1.0f);
  const float chevron = px(14);
  const ImRect text_bb(bb.Min + ImVec2(px(12), 0), ImVec2(bb.Max.x - px(12) - chevron - px(8), bb.Max.y));
  ImGui::PushClipRect(text_bb.Min, text_bb.Max, true);
  const ImVec2 ts = measure(value);
  draw_text(dl, ImVec2(text_bb.Min.x, std::round(bb.Min.y + (bb.GetHeight() - ts.y) * 0.5f)), theme::kChalk, value);
  ImGui::PopClipRect();
  theme::draw_icon(dl, theme::Icon::ChevronDown, ImVec2(bb.Max.x - px(12) - chevron * 0.5f, bb.GetCenter().y), chevron,
                   col(theme::kDim));
}

// A setting row's frame: the label and help on the left, `control` on the
// right, one navigation stop.
struct RowFrame {
  ImGuiID id = 0;
  bool ok = false;
  bool hovered = false, held = false, pressed = false, focus = false;
  ImRect bb, control;
  Keys keys;
};

RowFrame row_begin(const Row& row, ImVec2 control_size, ImGuiButtonFlags flags = 0, bool interactive = true) {
  RowFrame f;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return f;
  const float pad = px(12), gap = px(24);
  const float avail = ImGui::GetContentRegionAvail().x;
  const float label_w = std::max(px(80), avail - 2 * pad - gap - control_size.x);
  float label_h = 0, help_h = 0;
  {
    Font lead(Weight::Regular, theme::kLead);
    label_h = measure(row.label, label_w).y;
  }
  if (row.help && *row.help) {
    Font help(Weight::Regular, theme::kHelp);
    help_h = measure(row.help, label_w).y + px(2);
  }
  const float content_h = label_h + help_h;
  const float h = std::max({px(48), content_h + px(14), control_size.y + px(12)});
  f.id = w->GetID(row.label);
  f.bb = ImRect(w->DC.CursorPos, w->DC.CursorPos + ImVec2(avail, h));
  g_last_item = {f.id, f.bb.Min, f.bb.Max};
  ImGui::ItemSize(f.bb.GetSize());
  if (!row.enabled) ImGui::BeginDisabled();
  if (!interactive) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  f.ok = ImGui::ItemAdd(f.bb, f.id);
  if (!interactive) ImGui::PopItemFlag();
  if (f.ok) {
    if (interactive) {
      f.pressed = ImGui::ButtonBehavior(f.bb, f.id, &f.hovered, &f.held, flags | ImGuiButtonFlags_NoHoveredOnFocus);
      f.focus = focused(f.id);
      if (row.enabled) f.keys = row_keys(f.id);
    }
    ImDrawList* dl = w->DrawList;
    if (f.focus) dl->AddRectFilled(f.bb.Min, f.bb.Max, col(theme::kPanel), px(10));
    else if (f.hovered) dl->AddRectFilled(f.bb.Min, f.bb.Max, col(theme::alpha(theme::kPanel, 0.5f)), px(10));
    const float y = std::round(f.bb.Min.y + (h - content_h) * 0.5f);
    {
      Font lead(Weight::Regular, theme::kLead);
      draw_text(dl, ImVec2(f.bb.Min.x + pad, y), theme::kChalk, row.label, label_w);
    }
    if (help_h > 0) {
      Font help(Weight::Regular, theme::kHelp);
      draw_text(dl, ImVec2(f.bb.Min.x + pad, y + label_h + px(2)), theme::kDim, row.help, label_w);
    }
    f.control = ImRect(ImVec2(f.bb.Max.x - pad - control_size.x, std::round(f.bb.GetCenter().y - control_size.y * 0.5f)),
                       ImVec2(f.bb.Max.x - pad, std::round(f.bb.GetCenter().y + control_size.y * 0.5f)));
  }
  return f;
}

void row_end(const Row& row) {
  if (!row.enabled) ImGui::EndDisabled();
}

// A row of a drop-down list's popup.
bool popup_row(const char* label, bool current) {
  const float h = px(30);
  Item it = begin_item(label, ImVec2(ImGui::GetContentRegionAvail().x, h));
  if (!it.ok) return false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (it.hovered || it.held || focused(it.id)) dl->AddRectFilled(it.bb.Min, it.bb.Max, col(theme::kPanelHover), px(6));
  const ImVec2 ts = measure(label);
  draw_text(dl, ImVec2(it.bb.Min.x + px(10), std::round(it.bb.Min.y + (h - ts.y) * 0.5f)),
            current ? theme::kMarquee : theme::kChalk, label);
  // The list opens on its current value (ImGui's SetItemDefaultFocus()
  // comes too late: the popup measures itself, clipped, on its first frame).
  if (current && g_list_focus_pending) {
    focus(g_last_item, GImGui->NavCursorVisible);
    ImGui::SetScrollHereY();
    g_list_focus_pending = false;
  }
  return it.pressed;
}

}  // namespace

float binding_chip_width(const char* name) {
  Font f(Weight::Regular, theme::kBody);
  return std::round(measure(name).x + px(12) + px(34));
}

// ---- Focus ----

LastItem last_item() { return g_last_item; }

void focus(const LastItem& item, bool show) {
  ImGuiContext& g = *GImGui;
  ImGuiWindow* window = g.CurrentWindow;
  if (!window || item.id == 0) return;
  ImGui::SetNavWindow(window);
  ImGui::SetNavID(item.id, ImGuiNavLayer_Main, g.CurrentFocusScopeId, ImGui::WindowRectAbsToRel(window, ImRect(item.min, item.max)));
  // A window that just appeared would focus its first item next frame.
  g.NavInitRequest = false;
  g.NavInitResult.ID = 0;
  g.NavAnyRequest = g.NavMoveScoringItems;
  if (show) {
    g.NavCursorVisible = true;
    g.NavHighlightItemUnderNav = true;
  }
}

// ---- Shapes ----

void focus_ring(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, ImU32 colour) {
  const float t = px(2);
  dl->AddRect(min - ImVec2(t, t), max + ImVec2(t, t), col(colour), rounding + t, 0, t);
}

bool focused(ImGuiID id) {
  ImGuiContext& g = *GImGui;
  return g.NavId == id && g.NavCursorVisible && g.NavWindow != nullptr;
}

// ---- Text ----

void text(const char* s, float size, ImU32 colour, Weight weight) {
  Font f(weight, size);
  ImGui::PushStyleColor(ImGuiCol_Text, col(colour));
  ImGui::TextUnformatted(s);
  ImGui::PopStyleColor();
}

void text_wrapped(const char* s, float size, ImU32 colour, Weight weight, float width) {
  Font f(weight, size);
  ImGui::PushStyleColor(ImGuiCol_Text, col(colour));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (width > 0 ? px(width) : ImGui::GetContentRegionAvail().x));
  ImGui::TextUnformatted(s);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

void gap(float units) { ImGui::Dummy(ImVec2(0, px(units))); }

// ---- Buttons ----

bool pill_button(const char* label, Kind kind, bool enabled, float min_width) {
  Font f(kind == Kind::Quiet ? Weight::Regular : Weight::Bold, theme::kBody);
  const ImVec2 ts = measure(label);
  const ImVec2 size(std::max(px(min_width), std::round(ts.x + px(32))), px(34));
  if (!enabled) ImGui::BeginDisabled();
  Item it = begin_item(label, size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    ImU32 fill = 0, ink = theme::kChalk;
    switch (kind) {
      case Kind::Quiet: fill = it.held ? theme::kPanelHover : it.hovered ? theme::kPanel : 0; break;
      case Kind::Primary:
        fill = it.held ? theme::kMarqueeDark : it.hovered ? theme::kMarqueeHover : theme::kMarquee;
        ink = theme::kInkOnMarquee;
        break;
      case Kind::Danger:
        fill = it.held ? theme::mix(theme::kRed, theme::kBlack, 0.15f) : it.hovered ? theme::mix(theme::kRed, theme::kChalk, 0.08f) : theme::kRed;
        break;
    }
    if (fill) dl->AddRectFilled(it.bb.Min, it.bb.Max, col(fill), r);
    if (kind == Kind::Quiet) dl->AddRect(it.bb.Min, it.bb.Max, col(theme::kLine), r, 0, 1.0f);
    draw_centred(dl, it.bb, ink, label);
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r, ring_colour(kind));
  }
  if (!enabled) ImGui::EndDisabled();
  return it.pressed;
}

bool big_button(const char* label, Kind kind, bool enabled, float width) {
  Font f(kind == Kind::Quiet ? Weight::Regular : Weight::Bold, kind == Kind::Quiet ? 17.0f : 18.0f);
  const ImVec2 size(width > 0 ? px(width) : ImGui::GetContentRegionAvail().x, px(48));
  if (!enabled) ImGui::BeginDisabled();
  Item it = begin_item(label, size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = px(12);
    ImU32 fill = 0, ink = theme::kChalk;
    switch (kind) {
      case Kind::Quiet: fill = it.held ? theme::kLine : it.hovered ? theme::kPanelHover : theme::kPanel; break;
      case Kind::Primary:
        fill = it.held ? theme::kMarqueeDark : it.hovered ? theme::kMarqueeHover : theme::kMarquee;
        ink = theme::kInkOnMarquee;
        break;
      case Kind::Danger:
        fill = it.held ? theme::mix(theme::kRed, theme::kBlack, 0.15f) : it.hovered ? theme::mix(theme::kRed, theme::kChalk, 0.08f) : theme::kRed;
        break;
    }
    dl->AddRectFilled(it.bb.Min, it.bb.Max, col(fill), r);
    draw_centred(dl, it.bb, ink, label);
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r, ring_colour(kind));
  }
  if (!enabled) ImGui::EndDisabled();
  return it.pressed;
}

bool nav_item(const char* label, bool selected) {
  Font f(selected ? Weight::Bold : Weight::Regular, theme::kLead);
  const ImVec2 size(ImGui::GetContentRegionAvail().x, px(40));
  Item it = begin_item(label, size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = px(10);
    if (selected) dl->AddRectFilled(it.bb.Min, it.bb.Max, col(theme::kPanel), r);
    else if (it.hovered || it.held) dl->AddRectFilled(it.bb.Min, it.bb.Max, col(theme::alpha(theme::kPanel, 0.5f)), r);
    if (selected) {
      const ImVec2 m(it.bb.Min.x, it.bb.GetCenter().y - px(9));
      dl->AddRectFilled(m, m + ImVec2(px(3), px(18)), col(theme::kMarquee), px(2));
    }
    const ImVec2 ts = measure(label);
    draw_text(dl, ImVec2(it.bb.Min.x + px(14), std::round(it.bb.Min.y + (size.y - ts.y) * 0.5f)),
              selected ? theme::kChalk : theme::kDim, label);
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r);
  }
  return it.pressed;
}

bool device_pill(const char* label, bool selected) {
  Font f(selected ? Weight::Bold : Weight::Regular, theme::kBody);
  const ImVec2 ts = measure(label);
  const ImVec2 size(std::round(ts.x + px(32)), px(36));
  Item it = begin_item(label, size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    const ImU32 fill = selected ? theme::kChalk : (it.hovered || it.held) ? theme::kPanelHover : theme::kPanel;
    dl->AddRectFilled(it.bb.Min, it.bb.Max, col(fill), r);
    draw_centred(dl, it.bb, selected ? theme::kAsphalt : theme::kChalk, label);
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r);
  }
  return it.pressed;
}

bool binding_chip(const char* name) {
  Font f(Weight::Regular, theme::kBody);
  const float icon = px(12);
  const ImVec2 ts = measure(name);
  const ImVec2 size(std::round(ts.x + icon + px(34)), px(30));
  Item it = begin_item(name, size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    const bool lit = it.hovered || it.held || focused(it.id);
    dl->AddRectFilled(it.bb.Min, it.bb.Max, col(it.held ? theme::kPanelHover : theme::kPanel), r);
    dl->AddRect(it.bb.Min, it.bb.Max, col(theme::kLine), r, 0, 1.0f);
    draw_text(dl, ImVec2(it.bb.Min.x + px(12), std::round(it.bb.Min.y + (size.y - ts.y) * 0.5f)), theme::kChalk, name);
    theme::draw_icon(dl, theme::Icon::Cross, ImVec2(it.bb.Max.x - px(10) - icon * 0.5f, it.bb.GetCenter().y), icon,
                     col(lit ? theme::kRed : theme::kFaint));
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r);
    if (it.hovered) ImGui::SetTooltip("Remove");
  }
  return it.pressed;
}

bool add_chip() {
  Font f(Weight::Regular, theme::kBody);
  const char* label = "Add";
  const float icon = px(12);
  const ImVec2 ts = measure(label);
  const ImVec2 size(std::round(ts.x + icon + px(30)), px(30));
  Item it = begin_item("##add", size);
  if (it.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    if (it.hovered || it.held) dl->AddRectFilled(it.bb.Min, it.bb.Max, col(it.held ? theme::kPanelHover : theme::kPanel), r);
    dl->AddRect(it.bb.Min, it.bb.Max, col(theme::kLine), r, 0, 1.0f);
    theme::draw_icon(dl, theme::Icon::Plus, ImVec2(it.bb.Min.x + px(11) + icon * 0.5f, it.bb.GetCenter().y), icon, col(theme::kDim));
    draw_text(dl, ImVec2(it.bb.Min.x + px(11) + icon + px(6), std::round(it.bb.Min.y + (size.y - ts.y) * 0.5f)), theme::kChalk, label);
    if (focused(it.id)) focus_ring(dl, it.bb.Min, it.bb.Max, r);
  }
  return it.pressed;
}

void waiting_chip(const char* message) {
  Font f(Weight::Regular, theme::kBody);
  const ImVec2 ts = measure(message);
  const ImVec2 size(std::round(ts.x + px(24)), px(30));
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return;
  const ImRect bb(w->DC.CursorPos, w->DC.CursorPos + size);
  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(bb, 0)) return;
  // Pulsing between full and 45% opacity, 600 ms each way.
  const float a = 0.725f + 0.275f * std::cos(static_cast<float>(ImGui::GetTime()) * 2 * kPi / 1.2f);
  const ImU32 yellow = theme::alpha(theme::kMarquee, a);
  ImDrawList* dl = w->DrawList;
  dl->AddRect(bb.Min, bb.Max, col(yellow), size.y * 0.5f, 0, 1.0f);
  draw_centred(dl, bb, yellow, message);
}

// ---- Setting rows ----

bool row_toggle(const Row& row, bool* value) {
  RowFrame f = row_begin(row, ImVec2(px(42), px(24)));
  bool changed = false;
  if (f.ok) {
    if (f.pressed) {
      *value = !*value;
      changed = true;
    }
    if (f.keys.left && *value) *value = false, changed = true;
    if (f.keys.right && !*value) *value = true, changed = true;
    draw_toggle(ImGui::GetWindowDrawList(), f.id, f.control.Min, *value, f.focus);
  }
  row_end(row);
  return changed;
}

bool row_slider(const Row& row, float* value, float min, float max, float step, const char* format) {
  const float track_w = px(200), value_w = px(56), handle = px(9);
  RowFrame f = row_begin(row, ImVec2(track_w + px(12) + value_w, px(24)), ImGuiButtonFlags_PressedOnClick);
  bool changed = false;
  if (f.ok) {
    ImGuiContext& g = *GImGui;
    const ImRect track(ImVec2(f.control.Min.x, f.control.GetCenter().y - px(2)), ImVec2(f.control.Min.x + track_w, f.control.GetCenter().y + px(2)));
    const ImRect hit(track.Min - ImVec2(handle, px(12)), track.Max + ImVec2(handle, px(12)));
    const float before = *value;
    // Dragging: a press that began over the track.
    if (f.held && hit.Contains(g.IO.MouseClickedPos[0])) {
      const float t = std::clamp((g.IO.MousePos.x - track.Min.x) / track_w, 0.0f, 1.0f);
      *value = min + t * (max - min);
    }
    if (f.keys.left) *value -= step;
    if (f.keys.right) *value += step;
    if (step > 0) *value = std::round(*value / step) * step;
    *value = std::clamp(*value, min, max);
    changed = *value != before;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = (max > min) ? (*value - min) / (max - min) : 0.0f;
    dl->AddRectFilled(track.Min, track.Max, col(theme::kLine), px(2));
    dl->AddRectFilled(track.Min, ImVec2(track.Min.x + t * track_w, track.Max.y), col(theme::kBlue), px(2));
    const ImVec2 c(std::round(track.Min.x + t * track_w), track.GetCenter().y);
    const bool dragging = f.held && hit.Contains(g.IO.MouseClickedPos[0]);
    dl->AddCircleFilled(c, handle, col(dragging ? theme::kChalk : theme::kHandle));
    if (f.focus) dl->AddCircle(c, handle + px(1), col(theme::kBlue), 0, px(2));
    char buf[32];
    std::snprintf(buf, sizeof(buf), format, static_cast<double>(*value));
    Font body(Weight::Regular, theme::kBody);
    const ImVec2 ts = measure(buf);
    draw_text(dl, ImVec2(track.Max.x + px(12), std::round(f.control.GetCenter().y - ts.y * 0.5f)), theme::kDim, buf);
  }
  row_end(row);
  return changed;
}

bool row_choice(const Row& row, int* index, const std::vector<std::string>& items, const std::function<void()>& on_open) {
  const int count = static_cast<int>(items.size());
  if (count > 0) *index = std::clamp(*index, 0, count - 1);
  const ImVec2 box(px(220), px(34));
  RowFrame f = row_begin(row, box);
  bool changed = false;
  if (f.ok) {
    ImGui::PushID(f.id);
    const bool open = ImGui::IsPopupOpen("list");
    if (f.pressed && !open) {
      if (on_open) on_open();
      ImGui::OpenPopup("list");
      g_list_focus_pending = true;
    }
    if (count > 0) {
      if (f.keys.left) *index = (*index + count - 1) % count, changed = true;
      if (f.keys.right) *index = (*index + 1) % count, changed = true;
    }
    Font body(Weight::Regular, theme::kBody);
    const char* value = count > 0 ? items[static_cast<std::size_t>(*index)].c_str() : "";
    draw_choice_box(ImGui::GetWindowDrawList(), f.control, value, f.hovered && !open, f.focus || open);
    // The list, under the box.
    ImGui::SetNextWindowPos(ImVec2(f.control.Min.x, f.control.Max.y + px(4)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(box.x, 0), ImVec2(box.x, px(320)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(4), px(4)));
    if (ImGui::BeginPopup("list", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
      for (int i = 0; i < count; ++i) {
        ImGui::PushID(i);
        if (popup_row(items[static_cast<std::size_t>(i)].c_str(), i == *index)) {
          if (i != *index) changed = true;
          *index = i;
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
  }
  row_end(row);
  return changed;
}

bool row_button(const Row& row, const char* button, Kind kind) {
  Font f(kind == Kind::Quiet ? Weight::Regular : Weight::Bold, theme::kBody);
  const ImVec2 ts = measure(button);
  const ImVec2 size(std::round(ts.x + px(32)), px(34));
  RowFrame fr = row_begin(row, size);
  if (fr.ok) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    const bool lit = fr.hovered || fr.held;
    ImU32 fill = 0, ink = theme::kChalk;
    switch (kind) {
      case Kind::Quiet: fill = fr.held ? theme::kPanelHover : lit ? theme::kPanel : 0; break;
      case Kind::Primary: fill = fr.held ? theme::kMarqueeDark : lit ? theme::kMarqueeHover : theme::kMarquee; ink = theme::kInkOnMarquee; break;
      case Kind::Danger: fill = fr.held ? theme::mix(theme::kRed, theme::kBlack, 0.15f) : theme::kRed; break;
    }
    if (fill) dl->AddRectFilled(fr.control.Min, fr.control.Max, col(fill), r);
    if (kind == Kind::Quiet) dl->AddRect(fr.control.Min, fr.control.Max, col(fr.focus ? theme::kBlue : theme::kLine), r, 0, fr.focus ? px(2) : 1.0f);
    else if (fr.focus) focus_ring(dl, fr.control.Min, fr.control.Max, r, theme::kChalk);
    draw_centred(dl, fr.control, ink, button);
  }
  row_end(row);
  return fr.pressed;
}

void row_text(const Row& row, const char* value) {
  Font f(Weight::Regular, theme::kBody);
  const ImVec2 ts = measure(value);
  RowFrame fr = row_begin(row, ImVec2(std::round(ts.x), px(24)), 0, false);  // nothing to do on it
  if (fr.ok) draw_text(ImGui::GetWindowDrawList(), ImVec2(fr.control.Min.x, std::round(fr.control.GetCenter().y - ts.y * 0.5f)), theme::kDim, value);
  row_end(row);
}

// ---- Groups ----

void section(const char* title, const char* note) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->DC.CursorPos.y > w->DC.CursorStartPos.y) gap(10);
  ImGui::Indent(px(12));
  text(title, theme::kTitle, theme::kChalk, Weight::Bold);
  if (note && *note) text_wrapped(note, theme::kBody, theme::kDim);
  ImGui::Unindent(px(12));
  gap(2);
}

void divider() {
  gap(4);
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const ImVec2 p = w->DC.CursorPos;
  w->DrawList->AddLine(ImVec2(p.x + px(12), p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x - px(12), p.y), col(theme::kLine));
  gap(6);
}

void info_box(const char* message, bool problem) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return;
  const ImU32 tint = problem ? theme::kRed : theme::kBlue;
  const float pad = px(12), icon = px(20), avail = ImGui::GetContentRegionAvail().x;
  Font body(Weight::Regular, theme::kBody);
  const float text_w = avail - 2 * pad - icon - px(10);
  const ImVec2 ts = measure(message, text_w);
  const ImVec2 size(avail, std::max(ts.y, icon) + 2 * pad);
  const ImRect bb(w->DC.CursorPos, w->DC.CursorPos + size);
  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(bb, 0)) return;
  ImDrawList* dl = w->DrawList;
  dl->AddRectFilled(bb.Min, bb.Max, col(theme::alpha(tint, 0.14f)), px(10));
  dl->AddRect(bb.Min, bb.Max, col(theme::alpha(tint, 0.5f)), px(10), 0, 1.0f);
  theme::draw_icon(dl, problem ? theme::Icon::Warning : theme::Icon::Info, ImVec2(bb.Min.x + pad + icon * 0.5f, bb.Min.y + pad + icon * 0.5f),
                   icon, col(tint));
  draw_text(dl, ImVec2(bb.Min.x + pad + icon + px(10), bb.Min.y + pad), theme::kChalk, message, text_w);
}

void key_hint(const char* key, const char* action) {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w->SkipItems) return;
  ImDrawList* dl = w->DrawList;
  ImVec2 cap_size;
  {
    Font small(Weight::Bold, theme::kSmall);
    cap_size = measure(key) + ImVec2(px(14), px(6));
  }
  Font help(Weight::Regular, theme::kHelp);
  const ImVec2 ts = measure(action);
  const ImVec2 size(cap_size.x + px(6) + ts.x, std::max(cap_size.y, ts.y));
  const ImRect bb(w->DC.CursorPos, w->DC.CursorPos + size);
  ImGui::ItemSize(size);
  if (!ImGui::ItemAdd(bb, 0)) return;
  const ImRect cap(bb.Min, bb.Min + cap_size);
  dl->AddRectFilled(cap.Min, cap.Max, col(theme::kPanel), px(6));
  dl->AddRect(cap.Min, cap.Max, col(theme::kLine), px(6), 0, 1.0f);
  {
    Font small(Weight::Bold, theme::kSmall);
    draw_centred(dl, cap, theme::kChalk, key);
  }
  draw_text(dl, ImVec2(cap.Max.x + px(6), std::round(bb.Min.y + (size.y - ts.y) * 0.5f)), theme::kDim, action);
}

}  // namespace app::widgets
