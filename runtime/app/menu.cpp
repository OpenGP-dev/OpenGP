// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/menu.h"

#include <utility>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <string_view>

namespace app {

namespace {

// How long a notice stays: time to read it (about 16 characters a second),
// within these bounds.
constexpr auto kNoticeTime = std::chrono::milliseconds(2500);
constexpr auto kNoticeTimeMax = std::chrono::seconds(15);
constexpr auto kNoticeTimePerChar = std::chrono::milliseconds(60);

// Width of the menu window at a UI scale of 1 (a 720-pixel-high window).
constexpr float kMenuWidth = 640.0f;
constexpr float kMenuHeight = 620.0f;

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

namespace {

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

}  // namespace

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
  const auto reading = kNoticeTimePerChar * static_cast<int>(std::min<std::size_t>(message.size(), 1000));
  const Clock::time_point until = Clock::now() + std::clamp<Clock::duration>(reading, kNoticeTime, kNoticeTimeMax);
  // The same message again (a save that keeps failing) stays up once.
  for (Notice& n : notices_) {
    if (n.text == message) {
      n.until = until;
      return;
    }
  }
  notices_.push_back({std::move(message), until});
  if (notices_.size() > 4) notices_.erase(notices_.begin());
}

bool Menu::visible() const { return open_ || paused_ || !notices_.empty(); }

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
    // A fixed size (the tabs' contents scroll), within the window.
    ImGui::SetNextWindowSize(ImVec2(std::min(kMenuWidth * scale_, io.DisplaySize.x * 0.96f),
                                    std::min(kMenuHeight * scale_, io.DisplaySize.y * 0.94f)),
                             ImGuiCond_Always);
    if (focus_) {
      ImGui::SetNextWindowFocus();
      focus_ = false;
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("OpenGP", nullptr, flags)) {
      ImGui::TextDisabled("%s", m.title);
      if (ImGui::BeginTabBar("tabs")) {
        // Room for the buttons and the line of keys below the tab.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                             style.ItemSpacing.y * 2 + style.SeparatorTextBorderSize;
        auto tab = [&](const char* name, auto&& contents) {
          if (!ImGui::BeginTabItem(name)) return false;
          ImGui::BeginChild("page", ImVec2(0, -footer));
          contents();
          ImGui::EndChild();
          ImGui::EndTabItem();
          return true;
        };
        tab("Game", [&] { game_tab(m); });
        tab("Video", [&] { video_tab(m); });
        tab("Sound", [&] { sound_tab(m); });
        if (m.input && !tab("Controls", [&] { controls_tab(m); }) && m.input->capturing()) {
          m.input->cancel_capture();  // left the tab
        }
        ImGui::EndTabBar();
      }
      ImGui::Spacing();
      ImGui::Separator();
      if (ImGui::Button("Resume")) set_open(false);
      ImGui::SameLine();
      if (ImGui::Button("Quit") && m.quit) m.quit();
      const std::string pause = pause_keys(m, true);
      hint("Esc: menu    %s%sF11: fullscreen    F12: screenshot", pause.c_str(), pause.empty() ? "" : ": pause    ");
    }
    ImGui::End();
  } else if (paused_) {
    draw_paused(m);
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
    ImGui::TextWrapped("The card is loaded in the virtual card reader.");
  } else if (held_exists) {
    ImGui::TextWrapped("You insert it when the game asks for your card.");
  } else if (!held.empty()) {
    ImGui::TextWrapped(
        "A blank card: when the game asks on startup \"Do you have Mario Card?\", answer No, Then create one in the following prompt. It is saved as "
        "\"%s\".",
        held.c_str());
  } else {
    ImGui::TextWrapped("Playing without a card. (You can still create one after the game ends.)");
  }
  hint("Cards are saved in %s", cards.dir().c_str());

  ImGui::SeparatorText("Camera");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  static constexpr std::pair<CameraSource, const char*> kSources[] = {
      {CameraSource::Off, "Off"}, {CameraSource::Webcam, "Webcam"}, {CameraSource::TestPattern, "Test pattern"}};
  const char* source_label = "Off";
  for (const auto& [source, text] : kSources)
    if (source == s.camera) source_label = text;
  if (ImGui::BeginCombo("Photos from", source_label)) {
    for (const auto& [source, text] : kSources)
      if (ImGui::Selectable(text, source == s.camera)) s.camera = source;
    ImGui::EndCombo();
  }
  if (s.camera == CameraSource::Webcam) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    const std::string current = s.webcam.empty() ? std::string("Default") : s.webcam;
    if (ImGui::BeginCombo("Webcam", current.c_str())) {
      if (ImGui::IsWindowAppearing()) webcams_ = m.webcams ? m.webcams() : std::vector<std::string>{};
      if (ImGui::Selectable("Default", s.webcam.empty())) s.webcam.clear();
      for (const std::string& name : webcams_)
        if (ImGui::Selectable(name.c_str(), name == s.webcam)) s.webcam = name;
      ImGui::EndCombo();
    }
  }
  ImGui::TextWrapped("The game takes your photo for your character. Now: %s.", m.camera_info.c_str());
  hint("Changes take effect when the game restarts.");

  link_section(m);

  ImGui::SeparatorText("Pause");
  ImGui::Checkbox("Pause while this menu is open", &s.pause_in_menu);
  ImGui::Checkbox("Pause when the window is unfocused", &s.pause_in_background);
  if (m.linked) hint("When linked to another cabinet, Pausing is disabled.");
  else if (const std::string keys = pause_keys(m); !keys.empty()) hint("%s pauses and resumes the game.", keys.c_str());

  ImGui::SeparatorText("Discord");
  ImGui::Checkbox("Show what you're playing", &s.rich_presence);
  if (s.rich_presence)
    hint("%s", m.discord ? "On your Discord profile: the game, character, cup, course and place."
                         : "Discord isn't running on this computer.");

  ImGui::SeparatorText("Arcade");
  ImGui::Checkbox("Free play", &s.free_play);
  if (m.test_menu) {
    if (ImGui::Button("Test menu")) m.test_menu();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    hint("Open the Operator's Menu (Will restart the game)");
  }
}

// Linked cabinets: this one's number, the local network, and who it sees.
void Menu::link_section(MenuModel& m) {
  Settings& s = *m.settings;
  const triforce::LinkStatus& link = m.link;
  ImGui::SeparatorText("Linked cabinets");
  static constexpr const char* kNumbers[] = {"The game's setting", "1", "2", "3", "4"};
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  if (ImGui::BeginCombo("Cabinet number", kNumbers[std::clamp(s.cabinet, 0, 4)])) {
    for (int n = 0; n <= 4; ++n)
      if (ImGui::Selectable(kNumbers[n], n == s.cabinet)) s.cabinet = n;
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Link over the local network", &s.lan);

  std::string linked, found;
  for (int n = 1; n <= 4; ++n) {
    if (link.connected[n]) linked += (linked.empty() ? "" : ", ") + std::to_string(n);
    else if (link.found[n]) found += (found.empty() ? "" : ", ") + std::to_string(n);
  }
  if (!link.on || link.cabinet == 0) {
    ImGui::TextWrapped("Not linked yet.");
  } else if (!link.error.empty()) {
    ImGui::TextWrapped("%s", link.error.c_str());
  } else if (link.clash) {
    ImGui::TextWrapped("Another cabinet is also number %d: give each one its own number.", link.cabinet);
  } else if (!linked.empty()) {
    ImGui::TextWrapped("This is cabinet %d, linked with %s.", link.cabinet, linked.c_str());
  } else if (!found.empty()) {
    ImGui::TextWrapped("This is cabinet %d. Found %s, linking...", link.cabinet, found.c_str());
  } else {
    ImGui::TextWrapped("This is cabinet %d, looking for other cabinets %s.", link.cabinet,
                       s.lan ? "on this computer and the local network" : "on this computer");
  }
  for (const std::string& other : link.incompatible)
    ImGui::TextWrapped("OpenGP at %s can't link with this one: every cabinet needs the same version.", other.c_str());
  hint("Give each OpenGP its own number. Changes take effect when the game restarts.");
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
  ImGui::Checkbox("Even frame pacing", &s.even_pacing);
  hint("Shows each frame on the game's own 60 Hz tick, as the cabinet's display does: evenly spaced, so smoother "
       "with FreeSync or G-Sync, and about half a frame later.");
  hint("%s", m.video_info.c_str());
  ImGui::TextWrapped(
      "Widescreen expands the viewport, some 2D elements are at their original aspect ratio and some are stretched to fit.");
}

void Menu::sound_tab(MenuModel& m) {
  Settings& s = *m.settings;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
  ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%");
}

void Menu::controls_tab(MenuModel& m) {
  using Kind = input::HostInput::DeviceKind;
  input::HostInput& in = *m.input;
  static constexpr std::pair<Kind, const char*> kKinds[] = {
      {Kind::Keyboard, "Keyboard"}, {Kind::Gamepad, "Gamepad"}, {Kind::Joystick, "Wheel / joystick"}};
  for (const auto& [kind, name] : kKinds) {
    if (kind != Kind::Keyboard) ImGui::SameLine();
    if (ImGui::RadioButton(name, controls_kind_ == kind)) {
      controls_kind_ = kind;
      in.cancel_capture();
    }
  }
  const Kind kind = controls_kind_;
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("bindings", 2, flags)) {
    ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 130.0f * scale_);
    ImGui::TableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthStretch);
    for (std::size_t control = 0; control < input::HostInput::kControlCount; ++control) {
      // Keys cannot hold the wheel half-way: they steer with "steer left/right".
      if (kind == Kind::Keyboard && input::HostInput::control_is_axis(control)) continue;
      ImGui::PushID(static_cast<int>(control));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(input::HostInput::control_label(control));
      ImGui::TableNextColumn();
      const std::vector<std::string> names = in.bindings(kind, control);
      for (std::size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton((names[i] + "  x").c_str())) in.remove_binding(kind, control, i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
        ImGui::PopID();
        ImGui::SameLine();
      }
      if (in.capturing() && capture_control_ == control) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s",
                           kind == Kind::Keyboard   ? "Press a key..."
                           : input::HostInput::control_is_axis(control) ? "Move a joystick..."
                                                                        : "Press a button or move a joystick...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) in.cancel_capture();
      } else if (ImGui::SmallButton("+")) {
        in.start_capture(kind, control);
        capture_control_ = control;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  hint("Click an input to remove it; + adds one (Esc to cancel).");

  ImGui::Spacing();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
  float strength = in.force_feedback() * 100.0f;
  if (ImGui::SliderFloat("Vibration strength", &strength, 0.0f, 200.0f, "%.0f%%")) in.set_force_feedback(strength / 100.0f);
  if (kind == Kind::Keyboard) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    float speed = in.steer_speed();
    if (ImGui::SliderFloat("Turn speed", &speed, 0.5f, 10.0f, "%.1f turns/s")) in.set_steer_speed(speed);
  } else {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    float deadzone = in.deadzone(kind) * 100.0f;
    if (ImGui::SliderFloat("Stick Deadzone", &deadzone, 0.0f, 50.0f, "%.0f%%")) in.set_deadzone(kind, deadzone / 100.0f);
  }
  bool background = in.background_input();
  if (ImGui::Checkbox("Use the controls in the background too", &background)) in.set_background_input(background);
  hint("%s", input::HostInput::background_keyboard()
                 ? "Every OpenGP window reacts, active or not: for driving several copies at once."
                 : "Gamepads reach every OpenGP window, active or not (the keyboard only the active one).");
  if (ImGui::Button("Restore defaults")) in.restore_defaults();
  hint("Saved in %s", in.path().c_str());
}

void Menu::draw_paused(MenuModel& m) {
  const ImGuiIO& io = ImGui::GetIO();
  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 80));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.75f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##paused", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Paused");
    ImGui::SetWindowFontScale(1.0f);
    const std::string keys = pause_keys(m);
    if (!keys.empty()) ImGui::TextDisabled("%s: resume", keys.c_str());
  }
  ImGui::End();
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
    // Long ones wrap at the menu's width, within the window.
    const float width = std::min(kMenuWidth * scale_, io.DisplaySize.x - 4 * pad);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    for (const Notice& n : notices_) ImGui::TextUnformatted(n.text.c_str());
    ImGui::PopTextWrapPos();
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
