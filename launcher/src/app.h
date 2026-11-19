// The launcher: state, input, background work, and the screens.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "game_config.h"
#include "game_finder.h"
#include "lan_discovery.h"
#include "lobby_server.h"
#include "player_name.h"
#include "subprocess.h"
#include "setup_steps.h"
#include "toolchain.h"
#include "ui.h"
#include "util.h"

// Restores the window and asks the system to make it the active one (Windows).
void BringToFront(SDL_Window* window);

struct LaunchOptions {
  std::string screenshot_dir;  // render the demo screens to image files, then quit
  std::string root;            // act as if the launcher were in this folder
};

enum class Action { Up, Down, Left, Right, Accept, Back, Alt, Extra, PrevTab, NextTab, Start, Count };

// Result slot for the system file/folder dialogs (their callback may run on another thread).
struct PickerSlot {
  std::mutex mu;
  bool done = false;
  std::optional<std::string> path;
};

class App {
 public:
  explicit App(const LaunchOptions& options);
  ~App();
  bool Init(SDL_Window* window);
  void HandleEvent(const SDL_Event& e);
  void Tick();  // background polling only (used while the window is minimized)
  void Frame(float dt);
  bool WantsQuit() const { return quit_; }
  bool GameRunning() const { return screen_ == Screen::Running; }
  void RequestQuit();

  // Screenshot mode: fixed example states for every screen.
  static int DemoCount();
  static const char* DemoName(int index);
  void LoadDemo(int index);

 private:
  enum class Screen { Main, Setup, Settings, Log, Running, Name, Online, Host, Join, Address };
  enum class PlayMode { Offline, Online, LanHost, LanJoin };
  enum class Row { Game, Tools, Source, Files, Build, Count };

  struct Job {
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    mutable std::mutex mu;
    std::string title, stage, error;
    std::vector<std::string> error_lines;
    float progress = -1.f;
    int step_index = 0, step_count = 0;
    Row row = Row::Count;  // setup row being worked on
    int done = 0, total = 0;
    bool ok = false;
    double started = 0;
    // Time-left estimate (UI thread only).
    int rate_total = -1, rate_done0 = 0;
    double rate_t0 = 0, eta = -1;
    std::function<void(bool)> on_done;
  };

  struct Scan {
    toolchain::Info tools;
    bool game_found = false;
    finder::Candidate game;
  };

  struct Dialog {
    std::string title, message;
    std::vector<std::string> options;
    int selected = 0;
    int back_choice = -1;  // what B/Esc picks (-1: just close)
    ImU32 accent = ui::col::kAccent;
    std::function<void(int)> on_choose;
  };

  struct MenuItem {
    std::string label, description;
    bool enabled = true;
    std::function<void()> action;
  };

  // Input
  bool Pressed(Action a) const { return pressed_[static_cast<size_t>(a)]; }
  void Press(Action a) { pressed_[static_cast<size_t>(a)] = true; }
  void UpdateHeldDirections(float dt);
  bool MouseMoved() const { return mouse_moved_; }
  bool MouseIn(float x, float y, float w, float h) const;
  bool Clicked() const { return mouse_clicked_; }
  ui::Glyphs Glyphs() const;

  // State
  void StartScan(bool search_everywhere);
  void PollScan();
  void RefreshState();
  bool SetupComplete() const;
  bool GameKnown() const { return game_found_; }

  // Work
  void StartJob(const std::string& title, std::function<bool(JobContext&, Job&)> work, std::function<void(bool)> on_done);
  void PollJob();
  void CancelJob();
  bool JobRunning() const { return job_.running.load(); }
  void RunSetup();
  void StartSetupJob(bool install_tools);
  void RunBuild(bool confirm);
  void RunExtract(bool confirm);
  void RunUpdate();
  void ConfirmInstallTools(std::function<void()> then);
  void Play();  // starts the game in play_mode_ (set by StartPlay)
  void StartPlay(PlayMode mode, std::string lan_server = {});
  bool AskForNameBeforePlay();
  void ApplyGameDefaults();
  void PollGame();
  void PickGameFolder();
  void PickGameIso();
  void PollPicker();
  void UseGameCopy(const std::string& path);

  // UI helpers
  void ShowDialog(Dialog d);
  void ShowMessage(const std::string& title, const std::string& message, ImU32 accent = ui::col::kAccent);
  void Toast(const std::string& text);
  void SetFullscreen(bool on);
  void Go(Screen s);

  // Screens (app_screens.cpp)
  void DrawMain(float dt);
  void DrawSetup(float dt);
  void DrawSettings(float dt);
  void DrawLog(float dt);
  void DrawRunning(float dt);
  void DrawName(float dt);
  void DrawOnline(float dt);
  void DrawHost(float dt);
  void DrawJoin(float dt);
  void DrawAddress(float dt);
  void DrawDialog(float dt);
  void DrawToast();
  void DrawJobPanel(float x, float y, float w, float h);
  std::vector<MenuItem> MainItems();
  std::vector<MenuItem> OnlineItems();
  // A vertical menu like the main menu's; runs the chosen item's action.
  void DrawMenu(const std::vector<MenuItem>& items, int& sel, float& anim, float x, float y0, float w, float row_h,
                float dt);
  std::string RowStatus(Row r, ImU32& color) const;
  std::string RowDetail(Row r) const;
  void ActivateRow(Row r);
  void StartSourceJob();
  std::string HeaderStatus() const;

  // LAN play (app_screens.cpp)
  void StartHosting();
  void StopHosting();
  void OpenAddressEntry();
  void AddressType(char c);
  bool AddressValid(std::string* text) const;
  void JoinLan(const std::string& address, uint16_t port, const std::string& name);
  std::vector<std::string> RecentHosts() const;  // "name|address:port"
  void AddRecentHost(const std::string& name, const std::string& address_port);

  // Player name (app_screens.cpp)
  void LoadPlayerName();
  void OpenNameEntry(bool then_play);
  void ShowNameQuestion();
  void NameType(char c);
  void NameDelete();
  void NameKey(int row, int col);
  bool SaveName();

  LaunchOptions options_;
  SDL_Window* window_ = nullptr;
  bool quit_ = false;
  bool demo_ = false;
  double now_ = 0;

  Layout layout_;
  util::Settings settings_;
  util::Log log_;
  GameConfig game_config_;

  // What is known about this PC
  toolchain::Info tools_;
  bool tools_known_ = false;
  finder::Candidate game_;
  bool game_found_ = false;
  bool source_ready_ = false, libs_ready_ = false, files_ready_ = false, build_ready_ = false;
  int64_t built_at_ = 0;
  size_t files_count_ = 0;
  uint64_t files_bytes_ = 0;
  std::string source_version_;
  std::string player_name_;      // as saved in the game's settings
  bool player_name_set_ = false;  // false: the game uses its default name

  std::thread scan_thread_;
  std::atomic<bool> scan_running_{false};
  std::atomic<bool> scan_cancel_{false};
  std::mutex scan_mu_;
  std::optional<Scan> scan_result_;
  bool first_run_ = false;

  Job job_;
  std::string last_error_, last_error_title_;
  std::vector<std::string> last_error_lines_;
  proc::Child game_process_;

  PickerSlot picker_;
  bool picker_open_ = false;

  // Navigation
  Screen screen_ = Screen::Main;
  Screen log_return_ = Screen::Main;
  int main_sel_ = 0;
  int setup_sel_ = 0;
  int settings_tab_ = 0;
  int settings_sel_[3] = {0, 0, 0};
  float anim_main_ = 0, anim_setup_ = 0, anim_settings_ = 0;
  float screen_fade_ = 1.f;
  double log_scroll_ = 0;
  bool log_follow_ = true;
  std::optional<Dialog> dialog_;

  // Online and LAN play
  PlayMode play_mode_ = PlayMode::Offline;
  std::string play_lan_server_;  // "address:port" for LAN play
  Screen play_return_ = Screen::Main;  // where to go when the game closes
  int online_sel_ = 0, host_sel_ = 0, join_sel_ = 0;
  float anim_online_ = 0, anim_host_ = 0, anim_join_ = 0;
  lobby::Server lan_server_;
  std::string lan_server_error_;
  lan::Scanner lan_scanner_;
  std::string addr_digits_ = "192168001000";  // 12 digits, shown as 192.168.001.000
  int addr_cursor_ = 0;
  double addr_bad_flash_ = 0;
  // Example data for screenshots
  lobby::Status demo_host_status_;
  std::vector<lan::HostInfo> demo_hosts_;

  // Player name screen
  std::string name_text_;
  std::string name_typed_;  // keyboard text since the last frame
  Screen name_return_ = Screen::Main;
  bool name_then_play_ = false;
  bool name_caps_ = true;
  bool name_caps_auto_ = true;  // caps was switched on for the first letter only
  int name_row_ = 0, name_col_ = 0;
  double name_full_flash_ = 0;
  std::string toast_;
  double toast_until_ = 0;

  // Input state
  std::array<bool, static_cast<size_t>(Action::Count)> pressed_{};
  bool held_[4] = {};  // up, down, left, right from d-pad / stick
  float repeat_timer_[4] = {};
  bool using_gamepad_ = false;
  SDL_GamepadType gamepad_type_ = SDL_GAMEPAD_TYPE_STANDARD;
  bool mouse_moved_ = false, mouse_clicked_ = false;
  float wheel_ = 0;
};
