#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "imgui.h"

namespace {

constexpr float kRepeatDelay = 0.36f;
constexpr float kRepeatRate = 0.085f;
constexpr Sint16 kStickThreshold = 16000;

double NowSeconds() { return double(SDL_GetTicksNS()) / 1e9; }

// Short commit id of a git checkout, read straight from .git (no git needed).
std::string ReadGitHead(const fs::path& repo) {
  std::string head;
  if (!util::ReadTextFile(repo / ".git" / "HEAD", head)) return {};
  head = util::Trim(head);
  if (head.rfind("ref: ", 0) == 0) {
    const std::string ref = head.substr(5);
    std::string id;
    if (util::ReadTextFile(repo / ".git" / util::U8Path(ref), id)) return util::Trim(id).substr(0, 7);
    std::string packed;
    if (util::ReadTextFile(repo / ".git" / "packed-refs", packed)) {
      for (const auto& line : util::SplitLines(packed))
        if (line.size() > 41 && line.compare(41, std::string::npos, ref) == 0) return line.substr(0, 7);
    }
    return {};
  }
  return head.substr(0, 7);
}

void SDLCALL OnDialogPicked(void* userdata, const char* const* filelist, int /*filter*/) {
  auto* slot = static_cast<PickerSlot*>(userdata);
  std::lock_guard lock(slot->mu);
  slot->done = true;
  if (filelist && filelist[0]) slot->path = std::string(filelist[0]);
}

}  // namespace

App::App(const LaunchOptions& options) : options_(options) {}

App::~App() {
  scan_cancel_ = true;
  if (scan_thread_.joinable()) scan_thread_.join();
  if (job_.thread.joinable()) {
    job_.cancel = true;
    job_.thread.join();
  }
}

bool App::Init(SDL_Window* window) {
  window_ = window;
  fs::path exe_dir;
  if (!options_.root.empty()) {
    exe_dir = util::U8Path(options_.root);
  } else {
    const char* base = SDL_GetBasePath();
    exe_dir = util::U8Path(base ? base : ".");
  }
  if (!exe_dir.has_filename()) exe_dir = exe_dir.parent_path();
  layout_ = DetectLayout(exe_dir);
  first_run_ = !util::IsFile(layout_.settings_path);
  settings_.Load(layout_.settings_path);
  if (!options_.screenshot_dir.empty()) {
    demo_ = true;
    return true;
  }
  log_.Open(layout_.log_path, false);
  if (settings_.GetBool("fullscreen", false)) SDL_SetWindowFullscreen(window_, true);
  RefreshState();
  StartScan(true);
  return true;
}

// --- Input -------------------------------------------------------------------------

void App::HandleEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      RequestQuit();
      break;
    case SDL_EVENT_KEY_DOWN: {
      using_gamepad_ = false;
      const bool first = !e.key.repeat;
      switch (e.key.key) {
        case SDLK_UP: case SDLK_W: Press(Action::Up); break;
        case SDLK_DOWN: case SDLK_S: Press(Action::Down); break;
        case SDLK_LEFT: case SDLK_A: Press(Action::Left); break;
        case SDLK_RIGHT: case SDLK_D: Press(Action::Right); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (first) Press(Action::Accept); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: if (first) Press(Action::Back); break;
        case SDLK_Y: if (first) Press(Action::Alt); break;
        case SDLK_X: if (first) Press(Action::Extra); break;
        case SDLK_Q: case SDLK_PAGEUP: Press(Action::PrevTab); break;
        case SDLK_E: case SDLK_PAGEDOWN: Press(Action::NextTab); break;
        case SDLK_F5: if (first) Press(Action::Start); break;
        case SDLK_F11: if (first) SetFullscreen(!settings_.GetBool("fullscreen", false)); break;
        default: break;
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
      if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) gamepad_type_ = SDL_GetGamepadType(g);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      if (SDL_Gamepad* g = SDL_GetGamepadFromID(e.gdevice.which)) SDL_CloseGamepad(g);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
      using_gamepad_ = true;
      if (SDL_Gamepad* g = SDL_GetGamepadFromID(e.gbutton.which)) gamepad_type_ = SDL_GetGamepadType(g);
      auto dpad = [&](int i, Action a) {
        Press(a);
        held_[i] = true;
        repeat_timer_[i] = kRepeatDelay;
      };
      switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: dpad(0, Action::Up); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dpad(1, Action::Down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dpad(2, Action::Left); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dpad(3, Action::Right); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: Press(Action::Accept); break;
        case SDL_GAMEPAD_BUTTON_EAST: Press(Action::Back); break;
        case SDL_GAMEPAD_BUTTON_NORTH: Press(Action::Alt); break;
        case SDL_GAMEPAD_BUTTON_WEST: Press(Action::Extra); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: Press(Action::PrevTab); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: Press(Action::NextTab); break;
        case SDL_GAMEPAD_BUTTON_START: Press(Action::Start); break;
        default: break;
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: held_[0] = false; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: held_[1] = false; break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: held_[2] = false; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: held_[3] = false; break;
        default: break;
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      mouse_moved_ = true;
      using_gamepad_ = false;
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      using_gamepad_ = false;
      if (e.button.button == SDL_BUTTON_LEFT) mouse_clicked_ = true;
      if (e.button.button == SDL_BUTTON_RIGHT) Press(Action::Back);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      wheel_ += e.wheel.y;
      break;
    default:
      break;
  }
}

void App::UpdateHeldDirections(float dt) {
  // Left stick acts like the d-pad, with the same repeat timing.
  static bool stick_prev[4] = {};
  bool stick[4] = {};
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]);
      if (!g) continue;
      const Sint16 x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
      if (y < -kStickThreshold) stick[0] = true;
      if (y > kStickThreshold) stick[1] = true;
      if (x < -kStickThreshold) stick[2] = true;
      if (x > kStickThreshold) stick[3] = true;
    }
    SDL_free(ids);
  }
  const Action dirs[4] = {Action::Up, Action::Down, Action::Left, Action::Right};
  for (int i = 0; i < 4; ++i) {
    if (stick[i] && !stick_prev[i]) {
      using_gamepad_ = true;
      Press(dirs[i]);
      repeat_timer_[i] = kRepeatDelay;
    }
    if (held_[i] || stick[i]) {
      repeat_timer_[i] -= dt;
      if (repeat_timer_[i] <= 0.f) {
        Press(dirs[i]);
        repeat_timer_[i] = kRepeatRate;
      }
    }
    stick_prev[i] = stick[i];
  }
}

bool App::MouseIn(float x, float y, float w, float h) const {
  const ImVec2 m = ui::ToCanvas(ImGui::GetIO().MousePos);
  return m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
}

ui::Glyphs App::Glyphs() const {
  if (!using_gamepad_) return ui::Glyphs::Keyboard;
  switch (gamepad_type_) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return ui::Glyphs::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return ui::Glyphs::Nintendo;
    default:
      return ui::Glyphs::Xbox;
  }
}

// --- Frame -------------------------------------------------------------------------

void App::Tick() {
  now_ = demo_ ? 12.0 : NowSeconds();
  if (demo_) return;
  PollScan();
  PollJob();
  PollPicker();
  PollGame();
}

void App::Frame(float dt) {
  Tick();
  UpdateHeldDirections(dt);
  ImGuiIO& io = ImGui::GetIO();
  ui::BeginFrame(io.DisplaySize, float(now_), Glyphs());
  ui::Background();

  // A dialog takes the input; the screen under it only draws.
  auto saved = pressed_;
  const bool saved_moved = mouse_moved_, saved_clicked = mouse_clicked_;
  const float saved_wheel = wheel_;
  if (dialog_) {
    pressed_.fill(false);
    mouse_moved_ = mouse_clicked_ = false;
    wheel_ = 0;
  }
  switch (screen_) {
    case Screen::Main: DrawMain(dt); break;
    case Screen::Setup: DrawSetup(dt); break;
    case Screen::Settings: DrawSettings(dt); break;
    case Screen::Log: DrawLog(dt); break;
    case Screen::Running: DrawRunning(dt); break;
  }
  if (screen_fade_ < 1.f) {
    ui::Rect(-200, 104, ui::kCanvasW + 400, 556, ui::WithAlpha(ui::col::Rgba(3, 6, 11), 1.f - screen_fade_));
    screen_fade_ = std::min(1.f, screen_fade_ + dt * 7.f);
  }
  if (dialog_) {
    pressed_ = saved;
    mouse_moved_ = saved_moved;
    mouse_clicked_ = saved_clicked;
    wheel_ = saved_wheel;
    DrawDialog(dt);
  }
  DrawToast();

  pressed_.fill(false);
  mouse_moved_ = mouse_clicked_ = false;
  wheel_ = 0;
}

void App::Go(Screen s) {
  if (s == screen_) return;
  if (s == Screen::Log) {
    log_return_ = screen_;
    log_follow_ = true;
  }
  if (s == Screen::Settings) game_config_.Load(layout_.GameConfigPath());
  if (s == Screen::Main || s == Screen::Setup) RefreshState();
  screen_ = s;
  screen_fade_ = demo_ ? 1.f : 0.f;
}

void App::RequestQuit() {
  if (JobRunning()) {
    Dialog d;
    d.title = "STOP AND QUIT?";
    d.message = "\"" + job_.title + "\" is still running. Quitting stops it; you can start it again later.";
    d.options = {"KEEP RUNNING", "STOP AND QUIT"};
    d.back_choice = 0;
    d.accent = ui::col::kWarn;
    d.on_choose = [this](int i) {
      if (i == 1) {
        job_.cancel = true;
        quit_ = true;
      }
    };
    ShowDialog(std::move(d));
    return;
  }
  quit_ = true;
}

void App::ShowDialog(Dialog d) { dialog_ = std::move(d); }

void App::ShowMessage(const std::string& title, const std::string& message, ImU32 accent) {
  Dialog d;
  d.title = title;
  d.message = message;
  d.options = {"OK"};
  d.back_choice = 0;
  d.accent = accent;
  ShowDialog(std::move(d));
}

void App::Toast(const std::string& text) {
  toast_ = text;
  toast_until_ = now_ + 2.6;
}

void App::SetFullscreen(bool on) {
  SDL_SetWindowFullscreen(window_, on);
  settings_.SetBool("fullscreen", on);
  settings_.Save();
}

// --- What is known about this PC ---------------------------------------------------

void App::StartScan(bool search_everywhere) {
  if (scan_running_) return;
  if (scan_thread_.joinable()) scan_thread_.join();
  scan_running_ = true;
  scan_cancel_ = false;
  const std::string remembered = settings_.Get("game_path");
  const Layout layout = layout_;
  const bool need_search = search_everywhere && !game_found_;
  scan_thread_ = std::thread([this, remembered, layout, need_search] {
    Scan s;
    s.tools = toolchain::Detect(layout.tools_dir);
    if (!remembered.empty()) {
      finder::Candidate c;
      if (finder::Check(util::U8Path(remembered), c) && c.IsTroy()) {
        s.game_found = true;
        s.game = c;
      }
    }
    if (!s.game_found && need_search) {
      finder::Candidate c;
      const std::vector<fs::path> exclude = {layout.source_dir, layout.data_dir};
      if (finder::Search(finder::DefaultRoots(layout.exe_dir), exclude, &scan_cancel_, c)) {
        s.game_found = true;
        s.game = c;
      }
    }
    {
      std::lock_guard lock(scan_mu_);
      scan_result_ = std::move(s);
    }
    scan_running_ = false;
  });
}

void App::PollScan() {
  std::optional<Scan> result;
  {
    std::lock_guard lock(scan_mu_);
    result.swap(scan_result_);
  }
  if (!result) return;
  tools_ = std::move(result->tools);
  tools_known_ = true;
  if (result->game_found) {
    const bool newly = !game_found_;
    game_ = result->game;
    game_found_ = true;
    if (settings_.Get("game_path") != util::PathStr(game_.path)) {
      settings_.Set("game_path", util::PathStr(game_.path));
      settings_.Save();
    }
    if (newly && !files_ready_) Toast("Found your copy of the game");
  }
  RefreshState();
  if (first_run_) {
    first_run_ = false;
    settings_.Save();
    if (!SetupComplete() && screen_ == Screen::Main) Go(Screen::Setup);
  }
}

void App::RefreshState() {
  source_ready_ = steps::SourceReady(layout_);
  libs_ready_ = steps::HelperLibsReady(layout_);
  files_count_ = 0;
  files_bytes_ = 0;
  files_ready_ = steps::GameFilesReady(layout_, &files_count_, &files_bytes_);
  build_ready_ = steps::BuildReady(layout_, &built_at_);
  source_version_ = steps::SourceVersion(layout_);
  if (source_version_.empty() && util::IsDir(layout_.source_dir / ".git")) {
    const std::string head = ReadGitHead(layout_.source_dir);
    if (!head.empty()) source_version_ = "main @ " + head;
  }
}

bool App::SetupComplete() const { return files_ready_ && build_ready_; }

// --- Background work ---------------------------------------------------------------

void App::StartJob(const std::string& title, std::function<bool(JobContext&, Job&)> work,
                   std::function<void(bool)> on_done) {
  if (JobRunning()) return;
  if (job_.thread.joinable()) job_.thread.join();
  {
    std::lock_guard lock(job_.mu);
    job_.title = title;
    job_.stage = "Starting";
    job_.error.clear();
    job_.error_lines.clear();
    job_.progress = -1.f;
    job_.step_index = job_.step_count = 0;
    job_.row = Row::Count;
    job_.done = job_.total = 0;
    job_.ok = false;
    job_.started = now_;
    job_.rate_total = -1;
    job_.eta = -1;
  }
  last_error_.clear();
  last_error_lines_.clear();
  job_.on_done = std::move(on_done);
  job_.cancel = false;
  job_.finished = false;
  job_.running = true;
  log_.Open(layout_.log_path, true);
  log_.Clear();
  log_.Printf("=== %s (Operation Troy launcher %s) ===", title.c_str(), OPTROY_LAUNCHER_VERSION);

  JobContext ctx;
  ctx.layout = layout_;
  ctx.log = &log_;
  ctx.cancel = &job_.cancel;
  ctx.build_jobs = settings_.GetInt("build_jobs", 0);
  ctx.progress = [this](float p) {
    std::lock_guard lock(job_.mu);
    job_.progress = p;
  };
  ctx.stage = [this](const std::string& s) {
    std::lock_guard lock(job_.mu);
    job_.stage = s;
  };
  ctx.counter = [this](int done, int total) {
    std::lock_guard lock(job_.mu);
    job_.done = done;
    job_.total = total;
  };
  job_.thread = std::thread([this, ctx, work = std::move(work)]() mutable {
    const bool ok = work(ctx, job_);
    {
      std::lock_guard lock(job_.mu);
      job_.ok = ok;
      job_.error = ok ? std::string() : (ctx.error.empty() ? std::string("Something went wrong. See the log.") : ctx.error);
      job_.error_lines = ctx.error_lines;
    }
    log_.Add(ok ? "=== Done ===" : "=== Stopped: " + job_.error + " ===");
    job_.finished = true;
  });
}

void App::PollJob() {
  if (!job_.running || !job_.finished) return;
  if (job_.thread.joinable()) job_.thread.join();
  job_.running = false;
  bool ok;
  {
    std::lock_guard lock(job_.mu);
    ok = job_.ok;
    if (!ok && !job_.cancel) {
      last_error_title_ = job_.title;
      last_error_ = job_.error;
      last_error_lines_ = job_.error_lines;
    }
  }
  RefreshState();
  StartScan(false);  // tools may have changed
  if (job_.on_done) {
    auto done = std::move(job_.on_done);
    job_.on_done = nullptr;
    done(ok);
  }
}

void App::CancelJob() {
  if (!JobRunning()) return;
  Dialog d;
  d.title = "STOP?";
  d.message = "Stop \"" + job_.title + "\"? Work already done is kept, so starting again continues from here.";
  d.options = {"KEEP GOING", "STOP"};
  d.back_choice = 0;
  d.accent = ui::col::kWarn;
  d.on_choose = [this](int i) {
    if (i == 1) job_.cancel = true;
  };
  ShowDialog(std::move(d));
}

void App::ConfirmInstallTools(std::function<void()> then) {
  Dialog d;
  d.title = "INSTALL BUILD TOOLS?";
  d.message =
      "The PC version is built on your computer, so it needs Microsoft's free Build Tools for Visual Studio "
      "(C++, clang and CMake; about 6 GB).\n\nMicrosoft's installer will open and may ask for permission. "
      "You accept Microsoft's license there.";
  d.options = {"INSTALL", "NOT NOW"};
  d.back_choice = 1;
  d.on_choose = [then = std::move(then)](int i) {
    if (i == 0) then();
  };
  ShowDialog(std::move(d));
}

void App::RunSetup() {
  if (JobRunning()) return;
  if (!game_found_ && !files_ready_) {
    PickGameFolder();
    return;
  }
  if (!tools_known_) {
    Toast("Still checking this PC...");
    return;
  }
  if (!build_ready_ && !tools_.ok) {
    if (tools_.can_install) {
      ConfirmInstallTools([this] { StartSetupJob(true); });
    } else {
      ShowMessage("BUILD TOOLS MISSING", tools_.Summary(), ui::col::kWarn);
    }
    return;
  }
  StartSetupJob(false);
}

void App::StartSetupJob(bool install_tools) {
  struct Step {
    Row row;
    std::string name;
    std::function<bool(JobContext&, toolchain::Info&)> run;
  };
  auto list = std::make_shared<std::vector<Step>>();
  if (install_tools) {
    list->push_back({Row::Tools, "Installing build tools", [](JobContext& ctx, toolchain::Info& tools) {
                       if (!steps::InstallBuildTools(ctx, tools)) return false;
                       const std::string installer_note = ctx.error;  // set if the quiet install failed
                       ctx.error.clear();
                       tools = toolchain::Detect(ctx.layout.tools_dir);
                       ctx.log->Add(tools.Summary());
                       if (!tools.ok)
                         ctx.error = (installer_note.empty() ? std::string() : installer_note + " ") +
                                     "Still missing: " + tools.Summary().substr(9) + ". Details: %TEMP%\\dd_setup_*.log";
                       return tools.ok;
                     }});
  }
  if (!source_ready_ || !libs_ready_)
    list->push_back({Row::Source, "Getting the recomp source",
                     [](JobContext& ctx, toolchain::Info& tools) { return steps::GetSource(ctx, tools); }});
  if (!files_ready_) {
    const fs::path copy = game_.path;
    list->push_back({Row::Files, "Extracting the game files",
                     [copy](JobContext& ctx, toolchain::Info&) { return steps::ExtractGame(ctx, copy); }});
  }
  if (!build_ready_)
    list->push_back({Row::Build, "Building the PC version",
                     [](JobContext& ctx, toolchain::Info& tools) { return steps::BuildGame(ctx, tools); }});
  if (list->empty()) {
    Go(Screen::Main);
    return;
  }
  toolchain::Info tools = tools_;
  StartJob(
      "SETUP",
      [list, tools](JobContext& ctx, Job& job) mutable {
        for (size_t i = 0; i < list->size(); ++i) {
          {
            std::lock_guard lock(job.mu);
            job.step_index = int(i) + 1;
            job.step_count = int(list->size());
            job.row = (*list)[i].row;
            job.progress = -1.f;
            job.done = job.total = 0;
          }
          ctx.log->Add("--- " + (*list)[i].name + " ---");
          if (!(*list)[i].run(ctx, tools)) return false;
          if (ctx.Cancelled()) {
            ctx.error = "Stopped.";
            return false;
          }
        }
        return true;
      },
      [this](bool ok) {
        if (ok) {
          Toast("Setup complete. Ready to play!");
          main_sel_ = 0;
          Go(Screen::Main);
        }
      });
}

void App::RunBuild(bool confirm) {
  if (JobRunning()) return;
  if (!tools_.ok) {
    RunSetup();
    return;
  }
  if (confirm && build_ready_) {
    Dialog d;
    d.title = "BUILD AGAIN?";
    d.message = "Builds the PC version again. Only parts that changed are recompiled, so this is usually quick.";
    d.options = {"BUILD", "CANCEL"};
    d.back_choice = 1;
    d.on_choose = [this](int i) {
      if (i == 0) RunBuild(false);
    };
    ShowDialog(std::move(d));
    return;
  }
  toolchain::Info tools = tools_;
  StartJob(
      "BUILD",
      [tools](JobContext& ctx, Job& job) mutable {
        {
          std::lock_guard lock(job.mu);
          job.row = Row::Build;
          job.step_index = job.step_count = 1;
        }
        return steps::BuildGame(ctx, tools);
      },
      [this](bool ok) {
        if (ok) Toast("Build finished");
      });
  Go(Screen::Setup);
}

void App::RunExtract(bool confirm) {
  if (JobRunning()) return;
  if (!game_found_) {
    PickGameFolder();
    return;
  }
  if (confirm && files_ready_) {
    Dialog d;
    d.title = "EXTRACT AGAIN?";
    d.message = "Copies every game file from your copy again (about " + util::FormatBytes(game_.total_bytes) +
                "). Only needed if files were damaged or deleted.";
    d.options = {"EXTRACT", "CANCEL"};
    d.back_choice = 1;
    d.on_choose = [this](int i) {
      if (i == 0) RunExtract(false);
    };
    ShowDialog(std::move(d));
    return;
  }
  const fs::path copy = game_.path;
  StartJob(
      "EXTRACT",
      [copy](JobContext& ctx, Job& job) {
        {
          std::lock_guard lock(job.mu);
          job.row = Row::Files;
          job.step_index = job.step_count = 1;
        }
        return steps::ExtractGame(ctx, copy);
      },
      [this](bool ok) {
        if (ok) Toast("Game files extracted");
      });
  Go(Screen::Setup);
}

void App::RunUpdate() {
  if (JobRunning()) return;
  if (!source_ready_) {
    RunSetup();
    return;
  }
  toolchain::Info tools = tools_;
  auto changed = std::make_shared<bool>(false);
  StartJob(
      "UPDATE",
      [tools, changed](JobContext& ctx, Job& job) mutable {
        {
          std::lock_guard lock(job.mu);
          job.row = Row::Source;
          job.step_index = job.step_count = 1;
        }
        return steps::Update(ctx, tools, changed.get());
      },
      [this, changed](bool ok) {
        if (ok) Toast(*changed ? "Updated and rebuilt" : "Already up to date");
      });
  Go(Screen::Setup);
}

// --- Playing -----------------------------------------------------------------------

void App::Play() {
  if (JobRunning()) {
    Toast("Wait for " + job_.title + " to finish");
    Go(Screen::Setup);
    return;
  }
  if (!SetupComplete()) {
    Go(Screen::Setup);
    return;
  }
  std::error_code ec;
  fs::create_directories(layout_.saves_dir, ec);
  proc::Command c;
  c.args = {util::PathStr(layout_.GameExe()), "--game_data_root=" + util::PathStr(layout_.game_dir),
            "--user_data_root=" + util::PathStr(layout_.saves_dir)};
  c.cwd = util::PathStr(layout_.build_dir);
  std::string err;
  if (!game_process_.Start(c, &err)) {
    ShowMessage("COULD NOT START THE GAME", err, ui::col::kBad);
    return;
  }
  Go(Screen::Running);
  SDL_MinimizeWindow(window_);
}

void App::PollGame() {
  if (screen_ != Screen::Running || game_process_.Running()) return;
  SDL_RestoreWindow(window_);
  SDL_RaiseWindow(window_);
  Go(Screen::Main);
  if (game_process_.ExitCode() != 0) {
    char code[32];
    std::snprintf(code, sizeof code, "%d (0x%08X)", game_process_.ExitCode(), unsigned(game_process_.ExitCode()));
    ShowMessage("THE GAME CLOSED WITH AN ERROR",
                std::string("Exit code ") + code + ". The game's own log is in " +
                    util::PathStr(layout_.build_dir / "logs") + ".",
                ui::col::kWarn);
  }
}

// --- Choosing the game copy --------------------------------------------------------

void App::PickGameFolder() {
  if (picker_open_) return;
  picker_open_ = true;
  {
    std::lock_guard lock(picker_.mu);
    picker_.done = false;
    picker_.path.reset();
  }
  const std::string start = game_found_ ? util::PathStr(game_.path) : util::PathStr(layout_.exe_dir);
  SDL_ShowOpenFolderDialog(OnDialogPicked, &picker_, window_, start.c_str(), false);
}

void App::PickGameIso() {
  if (picker_open_) return;
  picker_open_ = true;
  {
    std::lock_guard lock(picker_.mu);
    picker_.done = false;
    picker_.path.reset();
  }
  static const SDL_DialogFileFilter kFilters[] = {{"Xbox 360 disc image (.iso)", "iso"}, {"All files", "*"}};
  const std::string start = util::PathStr(layout_.exe_dir);
  SDL_ShowOpenFileDialog(OnDialogPicked, &picker_, window_, kFilters, 2, start.c_str(), false);
}

void App::PollPicker() {
  if (!picker_open_) return;
  std::optional<std::string> path;
  {
    std::lock_guard lock(picker_.mu);
    if (!picker_.done) return;
    path = picker_.path;
  }
  picker_open_ = false;
  if (path) UseGameCopy(*path);
}

void App::UseGameCopy(const std::string& path) {
  finder::Candidate c;
  if (!finder::Check(util::U8Path(path), c)) {
    ShowMessage("NOT A GAME COPY",
                "Nothing usable was found there: " + c.error +
                    ".\n\nChoose the folder of your Games on Demand copy (the one that contains 424107D6), "
                    "or your .iso file.",
                ui::col::kWarn);
    return;
  }
  if (!c.IsTroy()) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X", c.title_id);
    ShowMessage("A DIFFERENT GAME",
                std::string("That is an Xbox 360 game, but not Gundam Operation Troy (its title ID is ") + buf +
                    ", not 424107D6).",
                ui::col::kWarn);
    return;
  }
  game_ = c;
  game_found_ = true;
  settings_.Set("game_path", util::PathStr(c.path));
  settings_.Save();
  Toast("Game copy set");
}
