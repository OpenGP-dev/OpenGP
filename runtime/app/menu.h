// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-game menu (Dear ImGui), drawn over the game: settings (video, sound,
// controls, game options, the Mario Card), opened with Esc or a gamepad's
// guide button. It also shows short notices ("Screenshot saved").
//
// The game gets no controls while it is open, and pauses unless the player
// chose otherwise or it is linked to other cabinets. The menu also says when
// the game is paused.
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "app/settings.h"
#include "input/host.h"
#include "triforce/card_folder.h"
#include "video/renderer.h"

struct SDL_Window;
union SDL_Event;

namespace app {

// What the menu shows and changes; the launcher applies the changes.
struct MenuModel {
  const char* title = "";          // the game's
  Settings* settings = nullptr;    // edited in place
  triforce::CardFolder* cards = nullptr;
  input::HostInput* input = nullptr;  // null without controls (their tab is hidden)
  std::string video_info;          // renderer, internal resolution
  std::function<std::vector<std::string>()> webcams;  // lists the webcams' names
  std::string camera_info;           // the camera this run uses
  bool linked = false;               // linked to another cabinet: no pausing
  bool discord = false;              // Discord is connected (for Rich Presence)
  std::function<void()> quit;
  std::function<void()> test_menu;  // restarts into the game's test menu (if it has one)
};

class Menu final : public video::Overlay {
 public:
  Menu() = default;
  ~Menu() override;
  Menu(const Menu&) = delete;
  Menu& operator=(const Menu&) = delete;

  // `format` is the window's SDL_GPUTextureFormat.
  bool init(SDL_Window* window, SDL_GPUDevice* device, u32 format, std::string* error);

  bool is_open() const { return open_; }
  void set_open(bool open);

  // Gives an event to the menu. Returns true if the game should not see it
  // (the menu is open, or the event opened or closed it).
  bool handle_event(const SDL_Event& event);

  // Shows a message for a few seconds.
  void notify(std::string message);

  // Shows that the game is paused (while the menu is closed).
  void set_paused(bool paused) { paused_ = paused; }

  // Whether there is anything to draw (the menu, a notice, or "Paused").
  bool visible() const;

  // Lays the menu out for the next presented frame.
  void update(MenuModel& model);

  // video::Overlay
  void prepare(SDL_GPUCommandBuffer* cmd) override;
  void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) override;

 private:
  using Clock = std::chrono::steady_clock;
  struct Notice {
    std::string text;
    Clock::time_point until;
  };

  void apply_scale(float scale);
  void game_tab(MenuModel& m);
  void video_tab(MenuModel& m);
  void sound_tab(MenuModel& m);
  void controls_tab(MenuModel& m);
  void draw_notices();
  void draw_paused(MenuModel& m);

  std::vector<std::string> webcams_;  // listed when the webcam list opens

  SDL_Window* window_ = nullptr;
  bool initialised_ = false;
  bool open_ = false;
  bool paused_ = false;
  bool focus_ = false;       // focus the menu window on its next frame
  bool rendered_ = false;    // ImGui::Render() ran for the next present
  float scale_ = 0.0f;       // UI scale in use
  std::vector<Notice> notices_;
  input::HostInput::DeviceKind controls_kind_ = input::HostInput::DeviceKind::Keyboard;
  std::size_t capture_control_ = 0;  // the control an input is being chosen for
};

}  // namespace app
