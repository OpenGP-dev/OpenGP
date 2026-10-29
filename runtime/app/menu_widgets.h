// SPDX-License-Identifier: GPL-3.0-or-later
//
// The menu's widgets, drawn in the launcher's look (its QML components:
// PillButton, GameButton, Toggle, ValueSlider, Choice, SettingRow, Section,
// Toast, ConfirmDialog, Icon). Each is a Dear ImGui item, so the keyboard,
// a gamepad and the mouse all work on it; each draws its own focus ring.
//
// Sizes are in the menu's units (theme::px()).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/menu_theme.h"

namespace app::widgets {

enum class Kind { Quiet, Primary, Danger };

// ---- Text ----

// A line of text at `size` units, in `weight`.
void text(const char* s, float size = theme::kBody, ImU32 colour = theme::kChalk,
          theme::Weight weight = theme::Weight::Regular);
// Text wrapped to the available width (or `width` units).
void text_wrapped(const char* s, float size = theme::kBody, ImU32 colour = theme::kDim,
                  theme::Weight weight = theme::Weight::Regular, float width = 0);
// A horizontal gap of `units` pixels (or vertical with `vertical`).
void gap(float units);

// ---- Buttons ----

// A pill (34 tall): quiet (outlined), primary (marquee yellow) or danger (red).
bool pill_button(const char* label, Kind kind = Kind::Quiet, bool enabled = true, float min_width = 0);
// A big rounded button (48 tall), the width given or the available one.
bool big_button(const char* label, Kind kind = Kind::Quiet, bool enabled = true, float width = 0);
// An item of the settings' page list (40 tall): the marker and bold text when `selected`.
bool nav_item(const char* label, bool selected);
// A device pill (36 tall): chalk when `selected`.
bool device_pill(const char* label, bool selected);
// A binding chip (30 tall): the name and a cross; returns true to remove it.
bool binding_chip(const char* name);
// "+ Add" chip; returns true to add a binding.
bool add_chip();
// The chip shown while an input is awaited (pulsing yellow).
void waiting_chip(const char* message);

// ---- Setting rows: label and help on the left, the control on the right.
// The whole row is one navigation stop: Left and Right change its value,
// A or Enter activates it, a click anywhere on it acts on its control. ----

struct Row {
  const char* label = "";
  const char* help = "";  // under the label, dim, wrapped
  bool enabled = true;
};

bool row_toggle(const Row& row, bool* value);
// A slider from `min` to `max` in steps; `format` prints the value (printf, a float).
bool row_slider(const Row& row, float* value, float min, float max, float step, const char* format);
// A drop-down list; `on_open` runs when its list opens (to refresh `items`).
bool row_choice(const Row& row, int* index, const std::vector<std::string>& items,
                const std::function<void()>& on_open = {});
// A row whose control is a quiet button.
bool row_button(const Row& row, const char* button, Kind kind = Kind::Quiet);
// A row with only a value on the right.
void row_text(const Row& row, const char* value);

// ---- Groups ----

// A heading (20 bold) and an optional note.
void section(const char* title, const char* note = nullptr);
// A hairline across the available width.
void divider();
// A tinted box with an icon: information (blue) or a problem (red).
void info_box(const char* message, bool problem = false);
// A keycap and what it does ("Esc", "Back"), on one line with the next ones.
void key_hint(const char* key, const char* action);

// ---- Shapes ----

// A 2-pixel focus ring outside `min`..`max`, at `rounding`.
void focus_ring(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding, ImU32 colour = theme::kBlue);
// Whether `id` is the keyboard's or the gamepad's focus (the ring is shown).
bool focused(ImGuiID id);

}  // namespace app::widgets
