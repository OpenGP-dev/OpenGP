// SPDX-License-Identifier: GPL-3.0-or-later
//
// The menu's look: the OpenGP launcher's palette and typeface (M PLUS Rounded
// 1c, embedded), the sizes in which it is drawn, and its few icons. The menu
// is designed in units for a 720-pixel-high window and scaled with it.
#pragma once

#include <imgui.h>

namespace app::theme {

// The palette, from the launcher's Theme.qml.
constexpr ImU32 kAsphalt = IM_COL32(0x1D, 0x23, 0x31, 255);      // the window
constexpr ImU32 kAsphaltDeep = IM_COL32(0x15, 0x1A, 0x25, 255);  // the page list, cards
constexpr ImU32 kPanel = IM_COL32(0x27, 0x2F, 0x41, 255);        // raised surfaces
constexpr ImU32 kPanelHover = IM_COL32(0x31, 0x3A, 0x4F, 255);
constexpr ImU32 kLine = IM_COL32(0x3A, 0x44, 0x59, 255);         // hairlines
constexpr ImU32 kMarquee = IM_COL32(0xFF, 0xC6, 0x29, 255);      // the one thing to press
constexpr ImU32 kMarqueeHover = IM_COL32(0xFF, 0xD2, 0x4F, 255);
constexpr ImU32 kMarqueeDark = IM_COL32(0xE0, 0xA8, 0x00, 255);
constexpr ImU32 kInkOnMarquee = IM_COL32(0x1B, 0x14, 0x05, 255);
constexpr ImU32 kBlue = IM_COL32(0x3B, 0x9B, 0xEF, 255);         // focus, switches, information
constexpr ImU32 kRed = IM_COL32(0xE5, 0x24, 0x3B, 255);          // danger, problems
constexpr ImU32 kGreen = IM_COL32(0x3F, 0xBF, 0x74, 255);        // linked
constexpr ImU32 kChalk = IM_COL32(0xF2, 0xF4, 0xF8, 255);        // text
constexpr ImU32 kDim = IM_COL32(0x98, 0xA3, 0xB6, 255);          // help
constexpr ImU32 kFaint = IM_COL32(0x6B, 0x76, 0x8A, 255);
constexpr ImU32 kHandle = IM_COL32(0xDD, 0xE3, 0xEC, 255);       // slider handles
constexpr ImU32 kBlack = IM_COL32(0, 0, 0, 255);

// `colour` at `a` (0 to 1) of its own opacity.
ImU32 alpha(ImU32 colour, float a);
// Between `a` and `b` (0: a, 1: b).
ImU32 mix(ImU32 a, ImU32 b, float t);

// Type sizes (units).
constexpr float kSmall = 12, kHelp = 13, kBody = 14, kLead = 16, kTitle = 20, kHeading = 26, kDisplay = 40;

// The scale of the frame being drawn: units to pixels.
void set_scale(float scale);
float scale();
float px(float units);

enum class Weight { Regular, Bold };
// The typeface at `size` units, until pop_font().
void push_font(Weight weight, float size);
void pop_font();

// Loads the typeface (once, at start-up).
void load_fonts(ImGuiIO& io);

// ImGui's own style for the menu at `scale`: what its windows, popups and
// scrollbars look like (the widgets are drawn by menu_widgets).
void apply_style(ImGuiStyle& style, float scale);

enum class Icon { Plus, Cross, ChevronDown, ChevronLeft, ChevronRight, Check, Info, Warning, Gear };
// An icon `size` pixels wide and high, centred at `centre`: round strokes,
// like the launcher's Icon.qml.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 centre, float size, ImU32 colour);

}  // namespace app::theme
