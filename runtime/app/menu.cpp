// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/menu.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <utility>

#include "app/menu_theme.h"
#include "app/menu_widgets.h"
#include "os/utf8.h"

namespace app {

namespace w = widgets;
using theme::px;

namespace {

// How long a notice stays: time to read it (about 16 characters a second),
// within these bounds.
constexpr auto kNoticeTime = std::chrono::milliseconds(2500);
constexpr auto kNoticeTimeMax = std::chrono::seconds(15);
constexpr auto kNoticeTimePerChar = std::chrono::milliseconds(60);
constexpr float kNoticeIn = 0.15f, kNoticeOut = 0.2f;  // seconds

constexpr float kFadeIn = 0.15f;  // the menu's, seconds

const char* const kPageNames[] = {"Game", "Video", "Sound", "Controls", "Camera", "Linked cabinets"};

// The keyboard keys bound to pause ("P / Pause"; with `first`, "P"), or ""
// if none.
std::string pause_keys(const MenuModel& m, bool first = false) {
  if (!m.input) return "";
  std::string keys;
  for (std::size_t c = 0; c < input::HostInput::kControlCount; ++c) {
    if (std::string_view(input::HostInput::control_name(c)) != "pause") continue;
    for (const std::string& key : m.input->bindings(input::HostInput::DeviceKind::Keyboard, c))
      if (keys.empty() || !first) keys += (keys.empty() ? "" : " / ") + key;
  }
  return keys;
}

// Opens a folder (UTF-8) in the system's file manager, making it first if
// needed. The menu shows a button for this rather than the folder's path,
// which would show the player's user name on screen and in screenshots.
bool open_folder(const std::string& folder) {
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(os::utf8_path(folder), ec);
  if (ec) return false;
  std::filesystem::create_directories(path, ec);
#ifdef _WIN32
  // ShellExecute (SDL_OpenURL on Windows) opens a plain path; a file:// URL
  // with non-ASCII characters is read in the wrong code page.
  const std::string url = os::utf8(path.lexically_normal().make_preferred());
#else
  // A file:// URL, with everything but unreserved characters and '/'
  // percent-encoded (macOS needs a valid URL).
  std::string url = "file://";
  for (const unsigned char c : os::utf8(path.lexically_normal())) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~' || c == '/') {
      url += static_cast<char>(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      url += hex;
    }
  }
#endif
  return SDL_OpenURL(url.c_str());
}

// Whether the last navigation came from a gamepad (the hints show its buttons).
bool using_gamepad() { return GImGui->NavInputSource == ImGuiInputSource_Gamepad; }

// What the first gamepad calls its face buttons: "A" and "B", or "Cross" and "Circle".
struct PadLabels {
  const char* select = "A";
  const char* back = "B";
};

PadLabels pad_labels() {
  // Once a frame: the hints ask more than once.
  static int frame = -1;
  static PadLabels cached;
  if (frame == ImGui::GetFrameCount()) return cached;
  frame = ImGui::GetFrameCount();
  PadLabels& labels = cached;
  labels = PadLabels{};
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids && count > 0) {
    const SDL_GamepadType type = SDL_GetGamepadTypeForID(ids[0]);
    auto name = [type](SDL_GamepadButton button, const char* fallback) {
      switch (SDL_GetGamepadButtonLabelForType(type, button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: return fallback;
      }
    };
    labels.select = name(SDL_GAMEPAD_BUTTON_SOUTH, "A");
    labels.back = name(SDL_GAMEPAD_BUTTON_EAST, "B");
  }
  SDL_free(ids);
  return labels;
}

// A rounded panel with a hairline border, behind the current window.
void window_panel(ImU32 fill, float rounding) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  dl->AddRectFilled(p, p + s, ImGui::GetColorU32(fill), rounding);
  dl->AddRect(p, p + s, ImGui::GetColorU32(theme::kLine), rounding, 0, 1.0f);
}

constexpr ImGuiWindowFlags kScreenFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;

}  // namespace

Menu::~Menu() {
  if (!initialised_) return;
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

bool Menu::init(SDL_Window* window, SDL_GPUDevice* device, u32 format, std::string* error) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // nothing to remember between runs
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                    ImGuiConfigFlags_NoMouseCursorChange;
  io.ConfigNavEscapeClearFocusItem = false;  // Esc goes back a level instead (update())
  if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
    *error = "could not start the menu (SDL3)";
    ImGui::DestroyContext();
    return false;
  }
  ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll, nullptr, 0);  // every pad drives the menu
  ImGui_ImplSDLGPU3_InitInfo info;
  info.Device = device;
  info.ColorTargetFormat = static_cast<SDL_GPUTextureFormat>(format);
  info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  if (!ImGui_ImplSDLGPU3_Init(&info)) {
    *error = "could not start the menu (SDL GPU)";
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  theme::load_fonts(io);
  window_ = window;
  initialised_ = true;
  return true;
}

void Menu::set_open(bool open) {
  if (open == open_) return;
  open_ = open;
  // The mouse cursor is ImGui's while the menu is open.
  ImGuiIO& io = ImGui::GetIO();
  if (open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  if (open) {
    screen_ = Screen::Card;
    confirm_ = Confirm::None;
    open_t_ = 0.0f;
    focus_window_ = focus_item_ = show_focus_ = true;
  } else {
    if (input_ && input_->capturing()) input_->cancel_capture();
    confirm_ = Confirm::None;
  }
}

bool Menu::handle_event(const SDL_Event& e) {
  if (!initialised_) return false;
  // Esc opens the menu; once it is open it goes back a level (update()).
  // The guide button always opens or closes it.
  const bool esc = e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat;
  const bool guide = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
  if (guide || (esc && !open_)) {
    set_open(!open_);
    return true;
  }
  // ImGui sees everything (so it knows which keys are down), but only takes
  // input while the menu is open.
  ImGui_ImplSDL3_ProcessEvent(&e);
  if (!open_) return false;
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: return true;
    default: return false;
  }
}

void Menu::notify(std::string message, NoticeKind kind) {
  const auto reading = kNoticeTimePerChar * static_cast<int>(std::min<std::size_t>(message.size(), 1000));
  const Clock::time_point now = Clock::now();
  const Clock::time_point until = now + std::clamp<Clock::duration>(reading, kNoticeTime, kNoticeTimeMax);
  // The same message again (a save that keeps failing) stays up once.
  for (Notice& n : notices_) {
    if (n.text == message) {
      n.until = until;
      n.kind = kind;
      return;
    }
  }
  notices_.push_back({std::move(message), kind, now, until});
  if (notices_.size() > 4) notices_.erase(notices_.begin());
}

bool Menu::visible() const { return open_ || paused_ || !notices_.empty(); }

void Menu::apply_scale(float scale) {
  theme::apply_style(ImGui::GetStyle(), scale);
  theme::set_scale(scale);
  scale_ = scale;
}

void Menu::block_gamepad_while_capturing(const MenuModel& m) {
  // While an input is being chosen for a control, and until every button is
  // up again afterwards, the gamepad does not navigate: binding B must not
  // also go back, nor binding A add another binding.
  // (The frame the capture ends on, the bound button is still down but
  // ImGui has not polled it yet: stay blocked until it has.)
  const bool capturing = m.input && m.input->capturing();
  if (capturing || was_capturing_) {
    pad_block_ = true;
  } else if (pad_block_) {
    bool down = false;
    for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickDown; ++k) down = down || ImGui::IsKeyDown(static_cast<ImGuiKey>(k));
    if (!down) pad_block_ = false;
  }
  was_capturing_ = capturing;
  ImGuiIO& io = ImGui::GetIO();
  if (pad_block_) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
  else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
}

bool Menu::back_pressed() const {
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return true;
  if (pad_block_) return false;
  const ImGuiIO& io = ImGui::GetIO();
  const ImGuiKey cancel = io.ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
  return ImGui::IsKeyPressed(cancel, false);
}

void Menu::go_back(MenuModel& m) {
  if (confirm_ != Confirm::None) {
    confirm_ = Confirm::None;
    return;
  }
  if (screen_ == Screen::Settings) {
    if (m.input && m.input->capturing()) m.input->cancel_capture();
    screen_ = Screen::Card;
    focus_window_ = focus_item_ = show_focus_ = true;
    return;
  }
  set_open(false);
}

void Menu::set_page(Page page, MenuModel& m, bool show_focus) {
  if (page == page_) return;
  if (page_ == Page::Controls && m.input && m.input->capturing()) m.input->cancel_capture();
  page_ = page;
  focus_item_ = scroll_top_ = true;
  show_focus_ = show_focus;
}

void Menu::take_focus() {
  if (!focus_item_) return;
  w::focus(w::last_item(), show_focus_);
  focus_item_ = false;
}

void Menu::update(MenuModel& m) {
  if (!initialised_) return;
  input_ = m.input;
  block_gamepad_while_capturing(m);
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  const ImGuiIO& io = ImGui::GetIO();
  // The menu grows with the window, like the game's own picture.
  const float scale = std::clamp(io.DisplaySize.y / 720.0f, 0.75f, 4.0f);
  if (std::abs(scale - scale_) > 0.01f) apply_scale(scale);
  theme::set_scale(scale_);
  ImGui::NewFrame();

  // A list left open when the menu closed (the guide button) must not come
  // back with it.
  if (!open_ && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
    ImGui::ClosePopupToLevel(0, false);
  if (open_) {
    open_t_ = std::min(1.0f, open_t_ + io.DeltaTime / kFadeIn);
    // Esc or B: one level up, unless ImGui just used it to close a list.
    if (back_pressed() && !popup_was_open_) go_back(m);
  }
  if (open_) {
    draw_scrims(open_t_);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open_t_);
    if (screen_ == Screen::Card) draw_card(m);
    else draw_settings(m);
    draw_confirm(m);
    ImGui::PopStyleVar();
    popup_was_open_ = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
                      confirm_ == Confirm::None;
  } else {
    popup_was_open_ = false;
    if (paused_) draw_paused(m);
  }
  draw_notices();
  ImGui::Render();
  rendered_ = true;
}

// ---- The screens ------------------------------------------------------------------------------

void Menu::draw_scrims(float alpha) {
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), size, theme::alpha(theme::kAsphalt, 0.45f * alpha));
  // Darker towards the bottom, like the launcher's hero pane.
  const ImU32 clear = theme::alpha(theme::kAsphaltDeep, 0.0f), deep = theme::alpha(theme::kAsphaltDeep, 0.85f * alpha);
  bg->AddRectFilledMultiColor(ImVec2(0, size.y * 0.55f), size, clear, clear, deep, deep);
  if (screen_ == Screen::Settings) bg->AddRectFilled(ImVec2(0, 0), size, theme::alpha(theme::kBlack, 0.35f * alpha));
}

void Menu::draw_card(MenuModel& m) {
  const ImGuiIO& io = ImGui::GetIO();
  const float width = std::min(px(400), io.DisplaySize.x - px(48));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
  if (focus_window_) {
    ImGui::SetNextWindowFocus();
    focus_window_ = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(24)));
  if (ImGui::Begin("##card", nullptr, kScreenFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
    window_panel(theme::alpha(theme::kAsphaltDeep, 0.94f), px(16));
    w::text(m.title, 22, theme::kChalk, theme::Weight::Bold);
    w::text(m.linked ? "Running: a linked game can't pause" : paused_ ? "Paused" : "Running", theme::kBody, theme::kDim);
    w::gap(10);
    if (w::big_button("Resume", w::Kind::Primary)) set_open(false);
    take_focus();
    if (w::big_button("Settings")) {
      screen_ = Screen::Settings;
      focus_window_ = focus_item_ = scroll_top_ = true;
      show_focus_ = GImGui->NavCursorVisible;
    }
    if (m.test_menu && w::big_button("Test menu")) confirm_ = Confirm::TestMenu;
    if (w::big_button("Quit")) confirm_ = Confirm::Quit;
    w::gap(6);
    draw_hints(false);
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void Menu::draw_hints(bool pages) {
  const bool pad = using_gamepad();
  const PadLabels labels = pad ? pad_labels() : PadLabels{};
  w::key_hint(pad ? labels.select : "Enter", "Select");
  ImGui::SameLine(0, px(14));
  w::key_hint(pad ? labels.back : "Esc", "Back");
  if (pages) {
    ImGui::SameLine(0, px(14));
    w::key_hint(pad ? "LB" : "PgUp", "");
    ImGui::SameLine(0, px(2));
    w::key_hint(pad ? "RB" : "PgDn", "Page");
  }
}

void Menu::draw_settings(MenuModel& m) {
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 size(std::min(px(940), io.DisplaySize.x - px(48)), std::min(px(640), io.DisplaySize.y - px(48)));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size);
  if (focus_window_) {
    ImGui::SetNextWindowFocus();
    focus_window_ = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const ImGuiWindowFlags flags = kScreenFlags | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::Begin("##settings", nullptr, flags)) {
    window_panel(theme::kAsphalt, px(16));
    // LB and RB, PageUp and PageDown: the previous and next page.
    {
      ImGuiContext& g = *GImGui;
      const ImGuiID id = g.CurrentWindow->ID;
      const bool pad = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
      const ImGuiInputFlags repeat = static_cast<int>(ImGuiInputFlags_Repeat) | static_cast<int>(ImGuiInputFlags_RepeatRateDefault);
      ImGui::SetKeyOwner(ImGuiKey_PageUp, id);
      ImGui::SetKeyOwner(ImGuiKey_PageDown, id);
      ImGui::SetKeyOwner(ImGuiKey_GamepadL1, id);
      ImGui::SetKeyOwner(ImGuiKey_GamepadR1, id);
      int step = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp, repeat, id) || (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, repeat, id))) step = -1;
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown, repeat, id) || (pad && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, repeat, id))) step = 1;
      if (step != 0 && confirm_ == Confirm::None) {
        int page = (static_cast<int>(page_) + step + kPageCount) % kPageCount;
        if (!m.input && page == static_cast<int>(Page::Controls)) page = (page + step + kPageCount) % kPageCount;
        set_page(static_cast<Page>(page), m, true);
      }
    }
    const ImVec2 origin = ImGui::GetWindowPos();
    const float nav_w = px(220);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, origin + ImVec2(nav_w, size.y), ImGui::GetColorU32(theme::kAsphaltDeep), px(16), ImDrawFlags_RoundCornersLeft);

    // The page list.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(18)));
    ImGui::BeginChild("##pages", ImVec2(nav_w, size.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Indent(px(10));
    w::text("Settings", theme::kHeading, theme::kChalk, theme::Weight::Bold);
    ImGui::Unindent(px(10));
    w::gap(8);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(10), px(4)));
    for (int i = 0; i < kPageCount; ++i) {
      if (i == static_cast<int>(Page::Controls) && !m.input) continue;
      if (w::nav_item(kPageNames[i], static_cast<int>(page_) == i)) set_page(static_cast<Page>(i), m, GImGui->NavCursorVisible);
    }
    ImGui::PopStyleVar();
    // The hints and the note sit at the bottom.
    {
      ImGui::Indent(px(10));
      const char* note = "Settings are shared with the launcher.";
      float note_h;
      {
        theme::push_font(theme::Weight::Regular, theme::kSmall);
        note_h = ImGui::CalcTextSize(note, nullptr, false, ImGui::GetContentRegionAvail().x).y;
        theme::pop_font();
      }
      const float hints_h = px(22) * 2 + px(8);
      const float bottom = ImGui::GetWindowContentRegionMax().y;
      ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), bottom - note_h - px(14) - hints_h));
      const bool pad = using_gamepad();
      const PadLabels labels = pad ? pad_labels() : PadLabels{};
      w::key_hint(pad ? labels.select : "Enter", "Select");
      ImGui::SameLine(0, px(12));
      w::key_hint(pad ? labels.back : "Esc", "Back");
      w::key_hint(pad ? "LB" : "PgUp", "");
      ImGui::SameLine(0, px(2));
      w::key_hint(pad ? "RB" : "PgDn", "Page");
      w::gap(4);
      w::text_wrapped(note, theme::kSmall, theme::kDim);
      ImGui::Unindent(px(10));
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // The page: its name and the way out, then its settings.
    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##right", ImVec2(size.x - nav_w, size.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
      ImGui::SetCursorPos(ImVec2(px(36), px(16)));
      w::text(kPageNames[static_cast<int>(page_)], theme::kHeading, theme::kChalk, theme::Weight::Bold);
      // Back and Resume, right-aligned on the same line.
      float back_w, resume_w;
      {
        theme::push_font(theme::Weight::Regular, theme::kBody);
        back_w = std::round(ImGui::CalcTextSize("Back").x + px(32));
        theme::pop_font();
        theme::push_font(theme::Weight::Bold, theme::kBody);
        resume_w = std::round(ImGui::CalcTextSize("Resume").x + px(32));
        theme::pop_font();
      }
      ImGui::SameLine(size.x - nav_w - px(16) - back_w - px(10) - resume_w, 0);
      ImGui::SetCursorPosY(px(16) + (px(26) * 1.3f - px(34)) * 0.5f);
      if (w::pill_button("Back")) go_back(m);
      ImGui::SameLine(0, px(10));
      if (w::pill_button("Resume", w::Kind::Primary)) set_open(false);
    }
    ImGui::SetCursorPos(ImVec2(0, px(16) + px(26) * 1.3f + px(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(8)));
    ImGui::BeginChild("##page", ImVec2(size.x - nav_w - px(8), ImGui::GetContentRegionAvail().y - px(12)), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (scroll_top_) {
      ImGui::SetScrollY(0.0f);
      scroll_top_ = false;
    }
    draw_page(m);
    w::gap(8);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

void Menu::draw_page(MenuModel& m) {
  switch (page_) {
    case Page::Game: page_game(m); break;
    case Page::Video: page_video(m); break;
    case Page::Sound: page_sound(m); break;
    case Page::Controls:
      if (m.input) page_controls(m);
      break;
    case Page::Camera: page_camera(m); break;
    case Page::Link: page_link(m); break;
  }
}

// ---- The pages --------------------------------------------------------------------------------

void Menu::page_game(MenuModel& m) {
  Settings& s = *m.settings;
  triforce::CardFolder& cards = *m.cards;

  w::section("Mario Card");
  const std::string held = cards.held();
  const bool held_exists = cards.exists(held);
  auto label = [&](const std::string& name) {
    if (name.empty()) return std::string("No card");
    return cards.exists(name) ? name : name + " (blank)";
  };
  std::vector<std::string> names, labels;
  names.push_back("");
  for (const std::string& name : cards.list()) names.push_back(name);
  if (!held.empty() && !held_exists) names.push_back(held);
  int index = 0;
  for (std::size_t i = 0; i < names.size(); ++i) {
    labels.push_back(label(names[i]));
    if (names[i] == held) index = static_cast<int>(i);
  }
  std::string status;
  if (cards.inserted()) status = "The card is in the virtual card reader.";
  else if (held_exists) status = "You insert it when the game asks for your card.";
  else if (!held.empty()) status = "A blank card: when the game asks \"Do you have Mario Card?\", answer No, then create one when it offers. It is saved as \"" + held + "\".";
  else status = "Playing without a card. You can still create one after the game ends.";
  if (w::row_choice({"Card", status.c_str()}, &index, labels)) {
    cards.hold(names[static_cast<std::size_t>(index)]);
    s.card = names[static_cast<std::size_t>(index)];
  }
  take_focus();
  if (w::row_button({"New card", "A blank card, for a new player: the game fills it in."}, "New card")) {
    const std::string name = cards.unused_name();
    cards.hold(name);
    s.card = name;
  }
  if (w::row_button({"Card files", "Each card is a file you can copy, back up or share."}, "Open folder"))
    open_folder(cards.dir());

  w::section("Pause");
  const std::string keys = pause_keys(m);
  const std::string pause_help = m.linked ? "A linked game can't pause." : keys.empty() ? "" : keys + " pauses and resumes the game.";
  w::row_toggle({"Pause while this menu is open", m.linked ? "A linked game can't pause." : "", !m.linked}, &s.pause_in_menu);
  w::row_toggle({"Pause when the window is in the background", pause_help.c_str(), !m.linked}, &s.pause_in_background);

  w::section("Discord");
  const char* discord_help = !s.rich_presence ? "" : m.discord ? "On your Discord profile: the game, character, cup, course and place."
                                                              : "Discord isn't running on this computer.";
  w::row_toggle({"Show what you're playing", discord_help}, &s.rich_presence);

  w::section("Arcade");
  w::row_toggle({"Free play", "Play without coins."}, &s.free_play);
}

void Menu::page_video(MenuModel& m) {
  Settings& s = *m.settings;
  if (w::row_toggle({"Full screen", "F11 or Alt+Enter switches too."}, &s.fullscreen)) {}
  take_focus();

  static const std::vector<std::string> kAspects = {"Fit the window", "4:3 (original)", "16:9", "16:10", "21:9"};
  static constexpr AspectRatio kAspectValues[] = {AspectRatio::Auto, AspectRatio::Original, AspectRatio::Wide16x9,
                                                  AspectRatio::Wide16x10, AspectRatio::Wide21x9};
  int aspect = 0;
  for (int i = 0; i < 5; ++i)
    if (kAspectValues[i] == s.aspect) aspect = i;
  if (w::row_choice({"Aspect ratio", "Widescreen expands the view; some 2D elements keep their shape, some stretch to fit."}, &aspect, kAspects))
    s.aspect = kAspectValues[aspect];

  std::vector<std::string> resolutions;
  resolutions.push_back("Match the window");
  for (u32 scale = 1; scale <= video::Renderer::kMaxScale; ++scale)
    resolutions.push_back(std::to_string(scale) + "x (" + std::to_string(640 * scale) + "x" + std::to_string(480 * scale) + ")");
  int resolution = static_cast<int>(std::min<u32>(s.internal_resolution, video::Renderer::kMaxScale));
  if (w::row_choice({"Resolution", "How sharply the game draws, as a multiple of the cabinet's own."}, &resolution, resolutions))
    s.internal_resolution = static_cast<u32>(resolution);

  w::row_toggle({"VSync", "Waits for the display's refresh: no tearing."}, &s.vsync);
  w::row_toggle({"Even frame pacing", "Shows each frame on the game's own 60 Hz tick, as the cabinet's display does: evenly spaced, so smoother with FreeSync or G-Sync, and about half a frame later."}, &s.even_pacing);
  w::row_text({"Renderer", ""}, m.video_info.c_str());
}

void Menu::page_sound(MenuModel& m) {
  Settings& s = *m.settings;
  float volume = static_cast<float>(s.volume);
  if (w::row_slider({"Volume", ""}, &volume, 0, 100, 1, "%.0f%%")) s.volume = static_cast<int>(std::lround(volume));
  take_focus();
}

void Menu::page_controls(MenuModel& m) {
  using Kind = input::HostInput::DeviceKind;
  input::HostInput& in = *m.input;
  static constexpr std::pair<Kind, const char*> kKinds[] = {
      {Kind::Keyboard, "Keyboard"}, {Kind::Gamepad, "Gamepad"}, {Kind::Joystick, "Wheel and other controllers"}};
  ImGui::Indent(px(12));
  for (const auto& [kind, name] : kKinds) {
    if (kind != Kind::Keyboard) ImGui::SameLine(0, px(6));
    if (w::device_pill(name, controls_kind_ == kind) && controls_kind_ != kind) {
      controls_kind_ = kind;
      in.cancel_capture();
    }
    if (kind == Kind::Keyboard) take_focus();
  }
  ImGui::Unindent(px(12));
  w::gap(6);
  const Kind kind = controls_kind_;

  // The cabinet's controls and what drives each.
  const float label_w = px(150), chip_h = px(30);
  for (std::size_t control = 0; control < input::HostInput::kControlCount; ++control) {
    // Keys cannot hold the wheel half-way: they steer with "steer left/right".
    if (kind == Kind::Keyboard && input::HostInput::control_is_axis(control)) continue;
    ImGui::PushID(static_cast<int>(control));
    const ImVec2 start = ImGui::GetCursorPos();
    {
      theme::push_font(theme::Weight::Regular, theme::kLead);
      const float text_h = ImGui::GetTextLineHeight();
      ImGui::SetCursorPos(ImVec2(start.x + px(12), start.y + std::round((chip_h - text_h) * 0.5f)));
      w::text(input::HostInput::control_label(control), theme::kLead);
      theme::pop_font();
    }
    const float chips_x = start.x + px(12) + label_w;
    ImGui::SetCursorPos(ImVec2(chips_x, start.y));
    const std::vector<std::string> names = in.bindings(kind, control);
    bool first = true;
    auto place = [&](float width) {
      if (!first) {
        ImGui::SameLine(0, px(6));
        if (ImGui::GetContentRegionAvail().x < width) {
          ImGui::NewLine();
          ImGui::SetCursorPosX(chips_x);
        }
      }
      first = false;
    };
    for (std::size_t i = 0; i < names.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      place(w::binding_chip_width(names[i].c_str()));
      if (w::binding_chip(names[i].c_str())) in.remove_binding(kind, control, i);
      ImGui::PopID();
    }
    if (in.capturing() && capture_control_ == control) {
      place(px(200));
      w::waiting_chip(kind == Kind::Keyboard                        ? "Press a key"
                      : input::HostInput::control_is_axis(control) ? "Move a stick or wheel"
                                                                   : "Press a button or move a stick");
      ImGui::SameLine(0, px(6));
      if (w::pill_button("Cancel")) in.cancel_capture();
    } else {
      place(px(80));
      if (w::add_chip()) {
        in.start_capture(kind, control);
        capture_control_ = control;
      }
    }
    ImGui::PopID();
  }

  w::divider();
  if (kind == Kind::Keyboard) {
    float speed = in.steer_speed();
    if (w::row_slider({"Turn speed", "How fast keys turn the wheel, in full turns a second."}, &speed, 0.5f, 10.0f, 0.5f, "%.1f"))
      in.set_steer_speed(speed);
  } else {
    float deadzone = in.deadzone(kind) * 100.0f;
    if (w::row_slider({"Stick dead zone", "How far a stick or wheel moves before the game reads it."}, &deadzone, 0, 50, 1, "%.0f%%"))
      in.set_deadzone(kind, deadzone / 100.0f);
    float strength = in.force_feedback() * 100.0f;
    if (w::row_slider({"Force feedback", "Wheels get the road's forces; gamepads rumble when they are strong."}, &strength, 0, 200, 5, "%.0f%%"))
      in.set_force_feedback(strength / 100.0f);
  }
  bool background = in.background_input();
  if (w::row_toggle({"Use the controls in the background too",
                     input::HostInput::background_keyboard()
                         ? "Every OpenGP window reacts, active or not: for driving several copies at once."
                         : "Gamepads reach every OpenGP window, active or not (the keyboard only the active one)."},
                    &background))
    in.set_background_input(background);
  if (w::row_button({"Bindings", "Every device goes back to OpenGP's defaults."}, "Restore defaults")) confirm_ = Confirm::RestoreDefaults;
  if (w::row_button({"Controls file", "Your bindings are kept in controls.toml."}, "Open folder"))
    open_folder(os::utf8(os::utf8_path(in.path()).parent_path()));
}

void Menu::page_camera(MenuModel& m) {
  Settings& s = *m.settings;
  if (m.restart_needed) {
    w::info_box("Changes take effect when the game restarts.");
    w::gap(4);
  }
  static const std::vector<std::string> kSources = {"Off", "Webcam", "Test pattern"};
  static constexpr CameraSource kSourceValues[] = {CameraSource::Off, CameraSource::Webcam, CameraSource::TestPattern};
  int source = 0;
  for (int i = 0; i < 3; ++i)
    if (kSourceValues[i] == s.camera) source = i;
  if (w::row_choice({"Photos from", "The game takes your photo for your character."}, &source, kSources)) s.camera = kSourceValues[source];
  take_focus();
  if (s.camera == CameraSource::Webcam) {
    std::vector<std::string> names;
    names.push_back("Default");
    for (const std::string& name : webcams_) names.push_back(name);
    int index = 0;
    for (std::size_t i = 0; i < webcams_.size(); ++i)
      if (webcams_[i] == s.webcam) index = static_cast<int>(i) + 1;
    if (!s.webcam.empty() && index == 0) {
      names.push_back(s.webcam);
      index = static_cast<int>(names.size()) - 1;
    }
    if (w::row_choice({"Webcam", ""}, &index, names, [&] { webcams_ = m.webcams ? m.webcams() : std::vector<std::string>{}; }))
      s.webcam = index == 0 ? std::string() : names[static_cast<std::size_t>(index)];
  }
  w::row_text({"This run's camera", ""}, m.camera_info.c_str());
}

void Menu::page_link(MenuModel& m) {
  Settings& s = *m.settings;
  const triforce::LinkStatus& link = m.link;
  if (m.restart_needed) {
    w::info_box("Changes take effect when the game restarts.");
    w::gap(4);
  }
  static const std::vector<std::string> kNumbers = {"The game's setting", "1", "2", "3", "4"};
  int cabinet = std::clamp(s.cabinet, 0, 4);
  if (w::row_choice({"Cabinet number", "Give each OpenGP its own number, 1 to 4."}, &cabinet, kNumbers)) s.cabinet = cabinet;
  take_focus();
  w::row_toggle({"Link over the local network", "With copies of OpenGP on other computers, not just this one."}, &s.lan);

  // Who it sees, with a dot: green linked, blue looking, red trouble.
  std::string linked, found;
  for (int n = 1; n <= 4; ++n) {
    if (link.connected[n]) linked += (linked.empty() ? "" : ", ") + std::to_string(n);
    else if (link.found[n]) found += (found.empty() ? "" : ", ") + std::to_string(n);
  }
  std::string status;
  ImU32 dot = theme::kFaint;
  if (!link.on || link.cabinet == 0) {
    status = "Not linked.";
  } else if (!link.error.empty()) {
    status = link.error;
    dot = theme::kRed;
  } else if (link.clash) {
    status = "Another cabinet is also number " + std::to_string(link.cabinet) + ": give each one its own number.";
    dot = theme::kRed;
  } else if (!linked.empty()) {
    status = "This is cabinet " + std::to_string(link.cabinet) + ", linked with " + linked + ".";
    dot = theme::kGreen;
  } else if (!found.empty()) {
    status = "This is cabinet " + std::to_string(link.cabinet) + ". Found " + found + ", linking...";
    dot = theme::kBlue;
  } else {
    status = "This is cabinet " + std::to_string(link.cabinet) + ", looking for other cabinets " +
             (s.lan ? "on this computer and the local network." : "on this computer.");
    dot = theme::kBlue;
  }
  w::gap(6);
  ImGui::Indent(px(12));
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float dot_r = px(5);
    theme::push_font(theme::Weight::Regular, theme::kBody);
    const float line_h = ImGui::GetTextLineHeight();
    theme::pop_font();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + dot_r, p.y + line_h * 0.5f), dot_r, ImGui::GetColorU32(dot));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dot_r * 2 + px(8));
    w::text_wrapped(status.c_str(), theme::kBody, theme::kChalk);
  }
  for (const std::string& other : link.incompatible) {
    const std::string line = "OpenGP at " + other + " can't link with this one: run the same version on every cabinet.";
    w::text_wrapped(line.c_str(), theme::kBody, theme::kDim);
  }
  ImGui::Unindent(px(12));
}

// ---- Confirmations ----------------------------------------------------------------------------

void Menu::draw_confirm(MenuModel& m) {
  const char* const kId = "##confirm";
  if (confirm_ == Confirm::None) {
    if (ImGui::IsPopupOpen(kId)) {
      // Closed by Esc or B: the popup follows.
      ImGui::SetNextWindowPos(ImVec2(-1000, -1000));
      if (ImGui::BeginPopupModal(kId, nullptr, kScreenFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
      }
    }
    return;
  }
  if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
  const ImGuiIO& io = ImGui::GetIO();
  const float width = std::min(px(460), io.DisplaySize.x - px(64));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(24)));
  if (ImGui::BeginPopupModal(kId, nullptr, kScreenFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
    window_panel(theme::kPanel, px(14));
    const char* heading = "";
    std::string text;
    const char* action = "";
    w::Kind kind = w::Kind::Primary;
    std::string title = m.title;
    switch (confirm_) {
      case Confirm::Quit:
        title = "Quit " + std::string(m.title) + "?";
        heading = title.c_str();
        text = "Your settings are saved. A race in progress ends, and the game closes.";
        action = "Quit";
        kind = w::Kind::Danger;
        break;
      case Confirm::TestMenu:
        heading = "Open the test menu?";
        text = "The game restarts into the operator's menu.";
        if (m.linked) text += " Linked cabinets lose the link.";
        action = "Test menu";
        break;
      case Confirm::RestoreDefaults:
        heading = "Restore the default controls?";
        text = "Every device's bindings go back to OpenGP's defaults.";
        action = "Restore";
        kind = w::Kind::Danger;
        break;
      case Confirm::None: break;
    }
    w::text_wrapped(heading, theme::kTitle, theme::kChalk, theme::Weight::Bold);
    w::gap(4);
    w::text_wrapped(text.c_str(), theme::kBody, theme::kDim);
    w::gap(10);
    float cancel_w, action_w;
    {
      theme::push_font(theme::Weight::Regular, theme::kBody);
      cancel_w = std::round(ImGui::CalcTextSize("Cancel").x + px(32));
      theme::pop_font();
      theme::push_font(theme::Weight::Bold, theme::kBody);
      action_w = std::round(ImGui::CalcTextSize(action).x + px(32));
      theme::pop_font();
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cancel_w - px(10) - action_w);
    if (w::pill_button("Cancel")) confirm_ = Confirm::None;
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine(0, px(10));
    if (w::pill_button(action, kind)) {
      const Confirm what = confirm_;
      confirm_ = Confirm::None;
      switch (what) {
        case Confirm::Quit:
          if (m.quit) m.quit();
          break;
        case Confirm::TestMenu:
          if (m.test_menu) m.test_menu();
          break;
        case Confirm::RestoreDefaults:
          if (m.input) m.input->restore_defaults();
          break;
        case Confirm::None: break;
      }
    }
    if (confirm_ == Confirm::None) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

// ---- Over the game ----------------------------------------------------------------------------

void Menu::draw_paused(MenuModel& m) {
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, theme::alpha(theme::kAsphalt, 0.3f));
  // A pill: "Paused", then the keys that resume.
  const std::string keys = pause_keys(m);
  std::vector<std::string> caps;
  for (std::size_t start = 0; start < keys.size();) {
    const std::size_t sep = keys.find(" / ", start);
    caps.push_back(keys.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
    if (sep == std::string::npos) break;
    start = sep + 3;
  }
  const float h = px(56), pad = px(28), gap = px(18);
  theme::push_font(theme::Weight::Bold, 24);
  const ImVec2 title = ImGui::CalcTextSize("Paused");
  theme::pop_font();
  float keys_w = 0;
  std::vector<ImVec2> cap_sizes;
  theme::push_font(theme::Weight::Bold, theme::kSmall);
  for (const std::string& cap : caps) {
    cap_sizes.push_back(ImGui::CalcTextSize(cap.c_str()) + ImVec2(px(14), px(6)));
    keys_w += cap_sizes.back().x + px(6);
  }
  theme::pop_font();
  theme::push_font(theme::Weight::Regular, theme::kHelp);
  const ImVec2 resume = ImGui::CalcTextSize("resumes");
  const ImVec2 orw = ImGui::CalcTextSize("or");
  theme::pop_font();
  if (!caps.empty()) keys_w += (caps.size() - 1) * (orw.x + px(6)) + resume.x;
  const float width = pad * 2 + title.x + (caps.empty() ? 0 : gap * 2 + 1 + keys_w);
  const ImVec2 min(std::round((io.DisplaySize.x - width) * 0.5f), std::round((io.DisplaySize.y - h) * 0.5f));
  const ImVec2 max = min + ImVec2(width, h);
  dl->AddRectFilled(min, max, theme::alpha(theme::kAsphaltDeep, 0.92f), h * 0.5f);
  dl->AddRect(min, max, theme::kLine, h * 0.5f, 0, 1.0f);
  float x = min.x + pad;
  theme::push_font(theme::Weight::Bold, 24);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, std::round(min.y + (h - title.y) * 0.5f)), theme::kChalk, "Paused");
  theme::pop_font();
  x += title.x;
  if (!caps.empty()) {
    x += gap;
    dl->AddLine(ImVec2(x, min.y + px(16)), ImVec2(x, max.y - px(16)), theme::kLine);
    x += gap;
    for (std::size_t i = 0; i < caps.size(); ++i) {
      const ImVec2 cmin(x, std::round(min.y + (h - cap_sizes[i].y) * 0.5f));
      dl->AddRectFilled(cmin, cmin + cap_sizes[i], theme::kPanel, px(6));
      dl->AddRect(cmin, cmin + cap_sizes[i], theme::kLine, px(6), 0, 1.0f);
      theme::push_font(theme::Weight::Bold, theme::kSmall);
      const ImVec2 ts = ImGui::CalcTextSize(caps[i].c_str());
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), cmin + ImVec2(std::round((cap_sizes[i].x - ts.x) * 0.5f), std::round((cap_sizes[i].y - ts.y) * 0.5f)),
                  theme::kChalk, caps[i].c_str());
      theme::pop_font();
      x += cap_sizes[i].x + px(6);
      theme::push_font(theme::Weight::Regular, theme::kHelp);
      const char* word = i + 1 < caps.size() ? "or" : "resumes";
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x, std::round(min.y + (h - resume.y) * 0.5f)), theme::kDim, word);
      x += (i + 1 < caps.size() ? orw.x : resume.x) + px(6);
      theme::pop_font();
    }
  }
}

void Menu::draw_notices() {
  const auto now = Clock::now();
  std::erase_if(notices_, [&](const Notice& n) { return n.until <= now; });
  if (notices_.empty()) return;
  const ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float margin = px(16), pad_y = px(14), pad_x = px(18), icon = px(20), gap = px(8);
  const float width = std::min(px(440), io.DisplaySize.x - 2 * margin);
  const float text_w = width - 2 * pad_x - icon - px(12);
  theme::push_font(theme::Weight::Regular, 15);
  // Newest at the bottom, the older ones stacked above it.
  float bottom = io.DisplaySize.y - margin;
  for (auto it = notices_.rbegin(); it != notices_.rend(); ++it) {
    const Notice& n = *it;
    const float in = std::min(1.0f, std::chrono::duration<float>(now - n.shown).count() / kNoticeIn);
    const float out = std::min(1.0f, std::chrono::duration<float>(n.until - now).count() / kNoticeOut);
    const float a = std::min(in, out);
    const ImVec2 ts = ImGui::CalcTextSize(n.text.c_str(), nullptr, false, text_w);
    const float h = std::max(ts.y, icon) + 2 * pad_y;
    const ImVec2 min(margin, std::round(bottom - h)), max(margin + width, bottom);
    const bool problem = n.kind == NoticeKind::Problem;
    dl->AddRectFilled(min, max, theme::alpha(theme::kPanel, 0.94f * a), px(12));
    dl->AddRect(min, max, theme::alpha(problem ? theme::kRed : theme::kLine, (problem ? 0.5f : 1.0f) * a), px(12), 0, 1.0f);
    theme::draw_icon(dl, problem ? theme::Icon::Warning : theme::Icon::Info,
                     ImVec2(min.x + pad_x + icon * 0.5f, min.y + pad_y + icon * 0.5f), icon,
                     theme::alpha(problem ? theme::kRed : theme::kBlue, a));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(min.x + pad_x + icon + px(12), min.y + pad_y),
                theme::alpha(theme::kChalk, a), n.text.c_str(), nullptr, text_w);
    bottom = min.y - gap;
  }
  theme::pop_font();
}

void Menu::prepare(SDL_GPUCommandBuffer* cmd) {
  if (!initialised_ || !rendered_) return;
  ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);
}

void Menu::draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
  if (!initialised_ || !rendered_) return;
  ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
  rendered_ = false;
}

}  // namespace app
