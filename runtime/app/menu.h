// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-game menu, drawn over the game with Dear ImGui in the OpenGP
// launcher's look: Esc or a gamepad's guide button opens a card (Resume,
// Settings, Test menu, Quit); Settings is the launcher's settings dialog
// (video, sound, the game, controls, the camera, linked cabinets). It also
// shows short notices ("Screenshot saved") and says when the game is paused.
//
// The game gets no controls while the menu is open, and pauses unless the
// player chose otherwise or it is linked to other cabinets.
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "app/profiler.h"
#include "app/settings.h"
#include "input/host.h"
#include "triforce/card_folder.h"
#include "triforce/network.h"
#include "video/renderer.h"

struct SDL_Window;
union SDL_Event;

namespace app {

// What the menu shows and changes; the launcher applies the changes.
struct MenuModel {
  const char* title = "";          // the game's
  Settings* settings = nullptr;    // edited in place
  triforce::CardFolder* cards = nullptr;
  input::HostInput* input = nullptr;  // null without controls (their page is hidden)
  std::string video_info;          // renderer, internal resolution
  std::function<std::vector<std::string>()> webcams;  // lists the webcams' names
  std::string camera_info;           // the camera this run uses
  bool linked = false;               // linked to another cabinet: no pausing
  triforce::LinkStatus link;         // what the link to other cabinets sees
  bool discord = false;              // Discord is connected (for Rich Presence)
  bool restart_needed = false;       // a camera or link setting differs from the running one
  std::function<void()> quit;
  std::function<void()> test_menu;  // restarts into the game's test menu (if it has one)
  // Profiling (app/profiler.h): whether a profile is being recorded, where,
  // how many frames so far, and the live numbers the overlay and its page
  // show; starting or stopping, marking a moment, the overlay and auto
  // captures.
  bool profiling = false;
  std::string profile_dir;
  Profiler::LiveStats profile;
  bool overlay = false;
  std::function<void()> toggle_profiling, mark, capture, toggle_overlay, toggle_auto_capture;
};

// A notice's kind: information, or a problem (a save that failed).
enum class NoticeKind { Info, Problem };

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

  // Shows a message for a few seconds, a long one for longer.
  void notify(std::string message, NoticeKind kind = NoticeKind::Info);

  // Shows that the game is paused (while the menu is closed).
  void set_paused(bool paused) { paused_ = paused; }

  // Shows the live profiler overlay over the game while the menu is closed.
  void set_profiler_overlay(bool on) { overlay_ = on; }

  // Whether there is anything to draw (the menu, a notice, "Paused", or the
  // profiler's overlay).
  bool visible() const;

  // Lays the menu out for the next presented frame.
  void update(MenuModel& model);

  // video::Overlay
  void prepare(SDL_GPUCommandBuffer* cmd) override;
  void draw(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) override;

 private:
  using Clock = std::chrono::steady_clock;
  enum class Screen { Card, Settings };
  enum class Page { Game, Video, Sound, Controls, Camera, Link, Profiler };
  static constexpr int kPageCount = 7;
  enum class Confirm { None, Quit, TestMenu, RestoreDefaults };
  struct Notice {
    std::string text;
    NoticeKind kind;
    Clock::time_point shown, until;
  };

  void apply_scale(float scale);
  // Keeps a gamepad being bound to a control from driving the menu.
  void block_gamepad_while_capturing(const MenuModel& m);
  bool back_pressed() const;  // Esc or B this frame
  void go_back(MenuModel& m);
  void set_page(Page page, MenuModel& m, bool show_focus);
  void take_focus();  // the last widget drawn gets the focus, if one is due
  void draw_scrims(float alpha);
  void draw_card(MenuModel& m);
  void draw_settings(MenuModel& m);
  void draw_page(MenuModel& m);
  void page_game(MenuModel& m);
  void page_video(MenuModel& m);
  void page_sound(MenuModel& m);
  void page_controls(MenuModel& m);
  void page_camera(MenuModel& m);
  void page_link(MenuModel& m);
  void page_profiler(MenuModel& m);
  void draw_confirm(MenuModel& m);
  void draw_hints(bool pages);
  void draw_paused(MenuModel& m);
  void draw_profiler_overlay(MenuModel& m);
  void draw_notices();

  std::vector<std::string> webcams_;  // listed when the webcam list opens

  SDL_Window* window_ = nullptr;
  input::HostInput* input_ = nullptr;  // the last model's, to cancel a capture on closing
  bool initialised_ = false;
  bool open_ = false;
  bool paused_ = false;
  bool overlay_ = false;  // the live profiler overlay, while the menu is closed
  bool focus_window_ = false;  // focus the screen's window on its next frame
  bool focus_item_ = false;    // focus the screen's first widget on its next frame
  bool show_focus_ = true;     // ... with its ring showing
  bool scroll_top_ = false;    // the page scrolls back to the top
  bool rendered_ = false;      // ImGui::Render() ran for the next present
  bool popup_was_open_ = false;  // a drop-down list was open last frame (Esc closes it, not the screen)
  bool pad_block_ = false;     // gamepad navigation is off (a capture)
  bool was_capturing_ = false; // an input was being chosen last frame
  float scale_ = 0.0f;         // UI scale in use
  float open_t_ = 0.0f;        // the fade-in, 0 to 1
  Screen screen_ = Screen::Card;
  Page page_ = Page::Game;
  Confirm confirm_ = Confirm::None;
  std::vector<Notice> notices_;
  input::HostInput::DeviceKind controls_kind_ = input::HostInput::DeviceKind::Keyboard;
  std::size_t capture_control_ = 0;  // the control an input is being chosen for
};

}  // namespace app
