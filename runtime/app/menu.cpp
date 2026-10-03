// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/menu.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace app {

namespace {

constexpr auto kNoticeTime = std::chrono::milliseconds(2500);

// Width of the menu window at a UI scale of 1 (a 720-pixel-high window).
constexpr float kMenuWidth = 600.0f;

// Greyed-out text, wrapped to the window.
void hint(const char* format, ...) IM_FMTARGS(1);
void hint(const char* format, ...) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  va_list args;
  va_start(args, format);
  ImGui::TextWrappedV(format, args);
  va_end(args);
  ImGui::PopStyleColor();
}

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
  if (!ImGui_ImplSDL3_InitForSDLGPU(window)) {
    *error = "could not start the menu (SDL3)";
    ImGui::DestroyContext();
    return false;
  }
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
  io.Fonts->AddFontDefaultVector();
  window_ = window;
  initialised_ = true;
  return true;
}

void Menu::set_open(bool open) {
  if (open == open_) return;
  open_ = open;
  focus_ = open;
  // The mouse cursor is ImGui's while the menu is open.
  ImGuiIO& io = ImGui::GetIO();
  if (open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

bool Menu::handle_event(const SDL_Event& e) {
  if (!initialised_) return false;
  const bool toggle = (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) ||
                      (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE);
  if (toggle) {
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

void Menu::notify(std::string message) {
  notices_.push_back({std::move(message), Clock::now() + kNoticeTime});
  if (notices_.size() > 4) notices_.erase(notices_.begin());
}

bool Menu::visible() const { return open_ || !notices_.empty(); }

void Menu::apply_scale(float scale) {
  // Sizes are scaled from the defaults each time (ScaleAllSizes multiplies).
  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 8.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.WindowPadding = ImVec2(16, 14);
  style.FramePadding = ImVec2(8, 5);
  style.ItemSpacing = ImVec2(10, 8);
  style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  style.Colors[ImGuiCol_WindowBg].w = 0.94f;
  style.ScaleAllSizes(scale);
  style.FontScaleMain = 1.35f;  // 13-pixel default font, about 18 pixels at 720p
  style.FontScaleDpi = scale;
  ImGui::GetStyle() = style;
  scale_ = scale;
}

void Menu::update(MenuModel& m) {
  if (!initialised_) return;
  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  const ImGuiIO& io = ImGui::GetIO();
  // The menu grows with the window, like the game's own picture.
  const float scale = std::clamp(io.DisplaySize.y / 720.0f, 0.75f, 4.0f);
  if (std::abs(scale - scale_) > 0.01f) apply_scale(scale);
  ImGui::NewFrame();

  if (open_) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kMenuWidth * scale_, 0.0f), ImGuiCond_Always);
    if (focus_) {
      ImGui::SetNextWindowFocus();
      focus_ = false;
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("OpenGP", nullptr, flags)) {
      ImGui::TextDisabled("%s", m.title);
      if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Game")) {
          game_tab(m);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Video")) {
          video_tab(m);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sound")) {
          sound_tab(m);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      ImGui::Spacing();
      ImGui::Separator();
      if (ImGui::Button("Resume")) set_open(false);
      ImGui::SameLine();
      if (ImGui::Button("Quit") && m.quit) m.quit();
      hint("Esc: menu    F11: fullscreen    F12: screenshot");
    }
    ImGui::End();
  }
  draw_notices();
  ImGui::Render();
  rendered_ = true;
}

void Menu::game_tab(MenuModel& m) {
  Settings& s = *m.settings;
  triforce::CardFolder& cards = *m.cards;

  ImGui::SeparatorText("Mario Card");
  const std::string held = cards.held();
  const bool held_exists = cards.exists(held);
  auto label = [&](const std::string& name) {
    if (name.empty()) return std::string("No card");
    return cards.exists(name) ? name : name + " (blank)";
  };
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  if (ImGui::BeginCombo("##card", label(held).c_str())) {
    auto option = [&](const std::string& name) {
      if (ImGui::Selectable(label(name).c_str(), name == held)) {
        cards.hold(name);
        s.card = name;
      }
    };
    option("");
    for (const std::string& name : cards.list()) option(name);
    if (!held.empty() && !held_exists) option(held);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("New card")) {
    const std::string name = cards.unused_name();
    cards.hold(name);
    s.card = name;
  }
  if (cards.inserted()) {
    ImGui::TextWrapped("The card is in the card reader.");
  } else if (held_exists) {
    ImGui::TextWrapped("You insert it when the game asks for your card.");
  } else if (!held.empty()) {
    ImGui::TextWrapped(
        "A blank card: when the game asks \"Do you have Mario Card?\", answer No, then buy one. It is saved as "
        "\"%s\".",
        held.c_str());
  } else {
    ImGui::TextWrapped("You play without a card (the game can still sell you one).");
  }
  hint("Cards are saved in %s", cards.dir().c_str());

  ImGui::SeparatorText("Arcade");
  ImGui::Checkbox("Free play (no coins needed)", &s.free_play);
}

void Menu::video_tab(MenuModel& m) {
  Settings& s = *m.settings;
  ImGui::Checkbox("Fullscreen (F11)", &s.fullscreen);

  static constexpr std::pair<AspectRatio, const char*> kAspects[] = {
      {AspectRatio::Auto, "Fit the window"}, {AspectRatio::Original, "4:3 (original)"}, {AspectRatio::Wide16x9, "16:9"},
      {AspectRatio::Wide16x10, "16:10"},     {AspectRatio::Wide21x9, "21:9"},
  };
  const char* current = "";
  for (const auto& [aspect, name] : kAspects)
    if (aspect == s.aspect) current = name;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  if (ImGui::BeginCombo("Aspect ratio", current)) {
    for (const auto& [aspect, name] : kAspects)
      if (ImGui::Selectable(name, aspect == s.aspect)) s.aspect = aspect;
    ImGui::EndCombo();
  }

  auto resolution = [](u32 scale) {
    if (scale == 0) return std::string("Match the window");
    return std::to_string(scale) + "x (" + std::to_string(640 * scale) + "x" + std::to_string(480 * scale) + ")";
  };
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  if (ImGui::BeginCombo("Resolution", resolution(s.internal_resolution).c_str())) {
    for (u32 scale = 0; scale <= video::Renderer::kMaxScale; ++scale)
      if (ImGui::Selectable(resolution(scale).c_str(), scale == s.internal_resolution)) s.internal_resolution = scale;
    ImGui::EndCombo();
  }
  ImGui::Checkbox("VSync", &s.vsync);
  hint("%s", m.video_info.c_str());
  ImGui::TextWrapped(
      "Widescreen shows more of the race on the sides; the game's 2D graphics stay in the middle, and full-screen "
      "backgrounds are stretched.");
}

void Menu::sound_tab(MenuModel& m) {
  Settings& s = *m.settings;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%");
}

void Menu::draw_notices() {
  const auto now = Clock::now();
  std::erase_if(notices_, [&](const Notice& n) { return n.until <= now; });
  if (notices_.empty()) return;
  const ImGuiIO& io = ImGui::GetIO();
  const float pad = 12.0f * scale_;
  ImGui::SetNextWindowPos(ImVec2(pad, io.DisplaySize.y - pad), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.6f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##notices", nullptr, flags)) {
    for (const Notice& n : notices_) ImGui::TextUnformatted(n.text.c_str());
  }
  ImGui::End();
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
