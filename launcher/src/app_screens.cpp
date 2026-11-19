// The launcher's screens. Layout is on the 1280x720 canvas (see ui.h).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"

namespace c = ui::col;
using ui::Button;
using ui::Font;

namespace {

float Approach(float current, float target, float dt) { return current + (target - current) * std::min(1.f, dt * 16.f); }
int Wrap(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }

const char* kRowTitles[] = {"GAME COPY", "BUILD TOOLS", "RECOMP SOURCE", "GAME FILES", "PC VERSION"};

const char* kRowInfo[] = {
    "Your own copy of Gundam Operation Troy: a Games on Demand (GOD) folder, or an Xbox 360 disc image (.iso). "
    "It is only read, never changed.",
    "The game's code is converted and compiled on this PC with Microsoft's free Build Tools (C++ and clang), "
    "CMake and Ninja.",
    "The recomp's own code (the ReXGlue SDK and this project), downloaded from GitHub. It contains no game code "
    "and no game files.",
    "The game's files are copied out of your copy into a folder the PC version reads.",
    "Your game's code is converted to C++ and compiled into the PC version. Everything stays on this PC.",
};

struct SettingRow {
  std::string label, description;
  std::vector<std::string> choices;  // empty: an action
  int current = 0;
  std::function<void(int)> set;
  std::function<void()> action;
};

}  // namespace

std::string App::HeaderStatus() const { return std::string("LAUNCHER ") + OPTROY_LAUNCHER_VERSION; }

// --- Main menu ---------------------------------------------------------------------

std::vector<App::MenuItem> App::MainItems() {
  std::vector<MenuItem> v;
  if (SetupComplete())
    v.push_back({"PLAY", "Start Gundam Operation Troy.", true, [this] { Play(); }});
  else
    v.push_back({"SET UP THE GAME", "Find your copy, extract it and build the PC version. Only needed once.", true,
                 [this] { Go(Screen::Setup); }});
  v.push_back({"ONLINE", "Online and LAN play are being rebuilt for the PC version. Coming in a later update.",
               false, nullptr});
  v.push_back({"SETTINGS", "Display options for the game, and launcher options.", true, [this] { Go(Screen::Settings); }});
  v.push_back({"UPDATE",
               source_ready_ ? "Get the newest version of the recomp and rebuild it." : "Available after setup.",
               source_ready_, [this] { RunUpdate(); }});
  v.push_back({"QUIT", "Close the launcher.", true, [this] { RequestQuit(); }});
  return v;
}

void App::DrawMain(float dt) {
  ui::Header("MAIN MENU", HeaderStatus(), false);
  auto items = MainItems();
  const int n = int(items.size());
  main_sel_ = std::clamp(main_sel_, 0, n - 1);
  if (Pressed(Action::Up)) main_sel_ = Wrap(main_sel_ - 1, n);
  if (Pressed(Action::Down)) main_sel_ = Wrap(main_sel_ + 1, n);
  if (Pressed(Action::Back)) main_sel_ = n - 1;

  const float x = 64, y0 = 146, row_h = 78, w = 530;
  bool activate = Pressed(Action::Accept) || Pressed(Action::Start);
  for (int i = 0; i < n; ++i) {
    if (MouseIn(x, y0 + float(i) * row_h, w, row_h - 8)) {
      if (MouseMoved()) main_sel_ = i;
      if (Clicked()) {
        main_sel_ = i;
        activate = true;
      }
    }
  }
  anim_main_ = demo_ ? float(main_sel_) : Approach(anim_main_, float(main_sel_), dt);
  ui::MenuHighlight(x - 8, y0 + anim_main_ * row_h - 2, w, row_h - 10, 1.f);
  for (int i = 0; i < n; ++i) {
    const MenuItem& it = items[size_t(i)];
    const float ry = y0 + float(i) * row_h;
    const bool sel = i == main_sel_;
    const ImU32 label_col = !it.enabled ? c::kTextFaint : sel ? c::kText : c::kTextDim;
    char num[8];
    std::snprintf(num, sizeof num, "%02d", i + 1);
    ui::Text(Font::Label, 13, x + 10, ry + 16, sel ? c::kAccent : c::kTextFaint, num);
    ui::Text(Font::Heading, 32, x + 44, ry + 4, label_col, it.label);
    if (sel)
      ui::Text(Font::Body, 15, x + 44, ry + 44, it.enabled ? c::kAccent : c::kTextFaint,
               ui::Fit(Font::Body, 15, it.description, w - 60));
    if (!it.enabled) ui::Chip(x + w - 26, ry + 24, "LATER", c::kTextFaint);
    if (sel) ui::Triangle(x + w - 18, ry + 24, 12, 0, ui::WithAlpha(c::kAccent, it.enabled ? 1.f : 0.3f));
  }
  if (activate && items[size_t(main_sel_)].enabled && items[size_t(main_sel_)].action) items[size_t(main_sel_)].action();
  if (Pressed(Action::Alt)) Go(Screen::Log);

  // Status panel
  const float px = 648, py = 150, pw = 568, ph = 386;
  ui::Panel(px, py, pw, ph, "STATUS");
  struct Info {
    const char* label;
    std::string value;
    ImU32 color;
  };
  std::vector<Info> rows;
  rows.push_back({"GAME COPY",
                  game_found_ ? util::ShortenPath(util::PathStr(game_.path), 58)
                              : (scan_running_ ? "Searching this PC..." : (files_ready_ ? "Not needed (files extracted)" : "Not found yet")),
                  game_found_ || files_ready_ ? c::kText : c::kWarn});
  rows.push_back({"BUILD TOOLS",
                  !tools_known_ ? "Checking..." : tools_.ok ? "clang " + tools_.clang_version + "  /  CMake " + tools_.cmake_version
                                                            : tools_.Summary(),
                  !tools_known_ || tools_.ok ? c::kText : (build_ready_ ? c::kTextDim : c::kWarn)});
  rows.push_back({"PC VERSION", build_ready_ ? "Built " + util::FormatTimestamp(built_at_) : "Not built yet",
                  build_ready_ ? c::kText : c::kWarn});
  rows.push_back({"RECOMP SOURCE",
                  source_ready_ ? (layout_.dev ? "Repository copy" : "Downloaded") +
                                      (source_version_.empty() ? std::string() : "  /  " + source_version_)
                                : "Not downloaded yet",
                  source_ready_ ? c::kText : c::kWarn});
  rows.push_back({"SAVES", util::ShortenPath(util::PathStr(layout_.saves_dir), 58), c::kText});
  for (size_t i = 0; i < rows.size(); ++i) {
    const float ry = py + 26 + float(i) * 60;
    ui::TextSpaced(Font::Label, 12, px + 28, ry, c::kTextFaint, rows[i].label, 2.f);
    ui::Text(Font::Body, 18, px + 28, ry + 18, rows[i].color, ui::Fit(Font::Body, 18, rows[i].value, pw - 56));
    if (i + 1 < rows.size()) ui::Line(px + 28, ry + 50, px + pw - 28, ry + 50, ui::WithAlpha(c::kPanelEdge, 0.6f));
  }
  if (JobRunning()) {
    float p;
    std::string title;
    {
      std::lock_guard lock(job_.mu);
      p = job_.progress;
      title = job_.title;
    }
    ui::TextSpaced(Font::Label, 12, px + 28, py + ph - 44, c::kAccent, title + " RUNNING", 2.f);
    ui::SegmentBar(px + 28, py + ph - 24, pw - 56, 8, p, c::kAccent);
  } else if (SetupComplete()) {
    ui::Chip(px + pw - 24, py + ph - 26, "READY TO PLAY", c::kGood);
  } else {
    ui::Chip(px + pw - 24, py + ph - 26, "SETUP NEEDED", c::kWarn);
  }
  ui::TextWrapped(Font::Body, 14, px + 4, py + ph + 22, pw - 8, c::kTextFaint,
                  "This launcher holds no game files. Your own copy of the game is converted and built on this PC.");

  ui::Footer(layout_.dev ? "Development copy: " + util::PathStr(layout_.source_dir) : std::string(),
             {{Button::Move, "SELECT"}, {Button::Accept, "OK"}, {Button::Alt, "BUILD LOG"}});
}

// --- Setup -------------------------------------------------------------------------

std::string App::RowStatus(Row r, ImU32& color) const {
  Row working = Row::Count;
  if (JobRunning()) {
    std::lock_guard lock(job_.mu);
    working = job_.row;
  }
  if (working == r) {
    color = c::kAccent;
    return "WORKING";
  }
  switch (r) {
    case Row::Game:
      if (game_found_) { color = c::kGood; return "FOUND"; }
      if (files_ready_) { color = c::kGood; return "OK"; }
      if (scan_running_) { color = c::kAccent; return "SEARCHING"; }
      color = c::kWarn;
      return "CHOOSE";
    case Row::Tools:
      if (!tools_known_) { color = c::kAccent; return "CHECKING"; }
      if (tools_.ok) { color = c::kGood; return "READY"; }
      color = build_ready_ ? c::kTextDim : c::kWarn;
      return "MISSING";
    case Row::Source:
      if (source_ready_ && libs_ready_) { color = c::kGood; return "READY"; }
      color = c::kWarn;
      return "NEEDED";
    case Row::Files:
      if (files_ready_) { color = c::kGood; return "DONE"; }
      color = c::kWarn;
      return "NEEDED";
    case Row::Build:
      if (build_ready_) { color = c::kGood; return "DONE"; }
      color = c::kWarn;
      return "NEEDED";
    default:
      color = c::kTextDim;
      return {};
  }
}

std::string App::RowDetail(Row r) const {
  switch (r) {
    case Row::Game:
      if (game_found_) return std::string(gdf::KindName(game_.kind)) + ":  " + util::PathStr(game_.path);
      if (files_ready_) return "Your game files are already extracted.";
      if (scan_running_) return "Looking in the usual places...";
      return "Choose the folder of your Games on Demand copy, or your .iso file.";
    case Row::Tools:
      if (!tools_known_) return "Checking this PC...";
      return tools_.Summary();
    case Row::Source:
      if (source_ready_ && !libs_ready_) return "The SDK's helper libraries still need downloading (about 400 MB).";
      if (source_ready_)
        return (layout_.dev ? "Repository copy" : "Downloaded") +
               (source_version_.empty() ? std::string() : "  /  " + source_version_);
      return "Downloaded from GitHub (about 400 MB, no game files).";
    case Row::Files:
      if (files_ready_)
        return files_count_ ? std::to_string(files_count_) + " files, " + util::FormatBytes(files_bytes_)
                            : "Extracted.";
      if (game_found_) return "About " + util::FormatBytes(game_.total_bytes) + " is copied from your game.";
      return "Needs your game copy first.";
    case Row::Build:
      if (build_ready_) return "Built " + util::FormatTimestamp(built_at_);
      return "The first build takes 30-90 minutes. Later builds are much quicker.";
    default:
      return {};
  }
}

void App::StartSourceJob() {
  toolchain::Info tools = tools_;
  StartJob(
      "DOWNLOAD",
      [tools](JobContext& ctx, Job& job) {
        {
          std::lock_guard lock(job.mu);
          job.row = Row::Source;
          job.step_index = job.step_count = 1;
        }
        return steps::GetSource(ctx, tools);
      },
      [this](bool ok) {
        if (ok) Toast("Recomp source ready");
      });
}

void App::ActivateRow(Row r) {
  if (JobRunning()) return;
  switch (r) {
    case Row::Game: PickGameFolder(); break;
    case Row::Tools:
      if (tools_known_ && !tools_.ok && tools_.can_install) {
        ConfirmInstallTools([this] { StartSetupJob(true); });
      } else {
        tools_known_ = false;
        StartScan(false);
      }
      break;
    case Row::Source:
      if (!source_ready_ || !libs_ready_) StartSourceJob();
      else Toast("The recomp source is ready. Use UPDATE on the main menu for newer versions.");
      break;
    case Row::Files: RunExtract(true); break;
    case Row::Build: RunBuild(true); break;
    default: break;
  }
}

void App::DrawJobPanel(float x, float y, float w, float h) {
  std::string title, stage;
  float progress;
  int step, steps, done, total;
  double started;
  {
    std::lock_guard lock(job_.mu);
    title = job_.title;
    stage = job_.stage;
    progress = job_.progress;
    step = job_.step_index;
    steps = job_.step_count;
    done = job_.done;
    total = job_.total;
    started = job_.started;
  }
  // Time left from the rate of finished items in the current phase.
  if (!demo_) {
    if (total != job_.rate_total) {
      job_.rate_total = total;
      job_.rate_done0 = done;
      job_.rate_t0 = now_;
      job_.eta = -1;
    } else if (total > 0 && done > job_.rate_done0 + 2 && now_ - job_.rate_t0 > 12.0) {
      const double rate = double(done - job_.rate_done0) / (now_ - job_.rate_t0);
      const double estimate = double(total - done) / rate;
      job_.eta = job_.eta < 0 ? estimate : job_.eta * 0.95 + estimate * 0.05;
    }
  }
  std::string panel_title = title;
  if (steps > 1) panel_title += "  /  STEP " + std::to_string(step) + " OF " + std::to_string(steps);
  ui::Panel(x, y, w, h, panel_title);
  ui::Spinner(x + 34, y + 37, 9, c::kAccent);
  ui::Text(Font::Heading, 22, x + 56, y + 22, c::kText, ui::Fit(Font::Heading, 22, stage, w - 220));
  if (progress >= 0.f) {
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", int(progress * 100.f));
    ui::Text(Font::Title, 34, x + w - 24, y + 12, c::kAccent, pct, 1.f);
  }
  ui::SegmentBar(x + 24, y + 62, w - 48, 12, progress, c::kAccent);
  std::string times = "Running for " + util::FormatDuration(now_ - started);
  if (job_.eta >= 0) times += "   /   About " + util::FormatDuration(job_.eta) + " left";
  ui::Text(Font::Body, 15, x + 24, y + 84, c::kTextDim, times);
  const float tw = ui::Measure(Font::Body, 15, times).x;
  ui::Text(Font::Mono, 13, x + w - 24, y + 86, c::kTextFaint,
           ui::Fit(Font::Mono, 13, log_.Last(), w - 90 - tw), 1.f);
}

void App::DrawSetup(float dt) {
  ui::Header("SETUP", HeaderStatus(), false);
  const bool busy = JobRunning();
  constexpr int kRows = int(Row::Count);
  const int count = busy ? kRows : kRows + 1;  // the last entry is the action button
  setup_sel_ = std::clamp(setup_sel_, 0, count - 1);
  if (Pressed(Action::Up)) setup_sel_ = Wrap(setup_sel_ - 1, count);
  if (Pressed(Action::Down)) setup_sel_ = Wrap(setup_sel_ + 1, count);

  const float x = 64, y0 = 130, row_h = 78, w = 760;
  bool activate = Pressed(Action::Accept);
  for (int i = 0; i < count; ++i) {
    const bool is_button = i == kRows;
    const float ry = is_button ? 552.f : y0 + float(i) * row_h;
    const float rh = is_button ? 64.f : row_h - 6;
    if (MouseIn(x, ry, w, rh)) {
      if (MouseMoved()) setup_sel_ = i;
      if (Clicked()) {
        setup_sel_ = i;
        activate = true;
      }
    }
  }

  ui::Panel(x - 10, y0 - 14, w + 20, float(kRows) * row_h + 18, "STEPS");
  if (setup_sel_ < kRows) {
    anim_setup_ = demo_ ? float(setup_sel_) : Approach(anim_setup_, float(setup_sel_), dt);
    ui::MenuHighlight(x, y0 + anim_setup_ * row_h + 2, w, row_h - 10, 1.f);
  } else {
    anim_setup_ = float(setup_sel_);
  }
  for (int i = 0; i < kRows; ++i) {
    const Row r = Row(i);
    const float ry = y0 + float(i) * row_h;
    const bool sel = i == setup_sel_;
    ImU32 sc;
    const std::string status = RowStatus(r, sc);
    ui::Chamfer(x + 16, ry + 16, 38, 38, 8, c::Rgba(12, 26, 42), ui::WithAlpha(sc, 0.7f), 1.5f);
    ui::Text(Font::Title, 22, x + 35, ry + 18, sc, std::to_string(i + 1), 0.5f);
    ui::TextSpaced(Font::Heading, 21, x + 72, ry + 10, sel ? c::kText : c::kTextDim, kRowTitles[i], 1.2f);
    ui::Text(Font::Body, 15, x + 72, ry + 42, sel ? c::kTextDim : c::kTextFaint,
             ui::Fit(Font::Body, 15, RowDetail(r), w - 230));
    ui::Chip(x + w - 18, ry + 30, status, sc);
    if (status == "WORKING" || status == "SEARCHING" || status == "CHECKING")
      ui::Spinner(x + w - 36 - ui::Measure(Font::Label, 13, status).x - 34, ry + 30, 7, sc);
    if (i + 1 < kRows) ui::Line(x + 72, ry + row_h - 4, x + w - 18, ry + row_h - 4, ui::WithAlpha(c::kPanelEdge, 0.5f));
  }

  // Right column: what the selected step is, or why setup stopped.
  const float ix = 848, iy = 116, iw = 368, ih = 412;
  if (!last_error_.empty() && !busy) {
    ui::Chamfer(ix, iy, iw, ih, 14, c::Rgba(40, 10, 14, 220), ui::WithAlpha(c::kBad, 0.8f), 1.f);
    ui::TextSpaced(Font::Heading, 20, ix + 22, iy + 20, c::kBad, last_error_title_ + " STOPPED", 1.4f);
    float yy = iy + 58;
    yy += ui::TextWrapped(Font::Body, 16, ix + 22, yy, iw - 44, c::kText, last_error_, 1.3f, 5) + 10;
    for (const auto& line : last_error_lines_) {
      if (yy > iy + ih - 60) break;
      yy += ui::TextWrapped(Font::Mono, 13, ix + 22, yy, iw - 44, c::Rgba(255, 170, 170), line, 1.25f, 3) + 6;
    }
    ui::Text(Font::Body, 14, ix + 22, iy + ih - 34, c::kTextDim, "The full log has every detail.");
  } else {
    ui::Panel(ix, iy, iw, ih, "INFO");
    const bool button = setup_sel_ == kRows;
    const std::string head = button ? "SETUP" : kRowTitles[setup_sel_];
    ui::TextSpaced(Font::Heading, 20, ix + 22, iy + 22, c::kAccent, head, 1.4f);
    const char* info = button ? "Runs every step that is still needed, in order. You can leave it running; "
                                "progress is shown here and in the build log."
                              : kRowInfo[setup_sel_];
    float yy = iy + 60;
    yy += ui::TextWrapped(Font::Body, 16, ix + 22, yy, iw - 44, c::kText, info) + 18;
    if (!button && !busy) {
      const Row r = Row(setup_sel_);
      std::string what;
      switch (r) {
        case Row::Game: what = "Choose a folder. A disc image works too."; break;
        case Row::Tools:
          what = tools_known_ && !tools_.ok && tools_.can_install ? "Install Microsoft's Build Tools (about 6 GB)."
                                                                   : "Check this PC again.";
          break;
        case Row::Source: what = source_ready_ && libs_ready_ ? "Nothing to do." : "Download it now."; break;
        case Row::Files: what = files_ready_ ? "Extract the files again." : "Extract the files now."; break;
        case Row::Build: what = build_ready_ ? "Build again (quick)." : "Build now."; break;
        default: break;
      }
      ui::TextSpaced(Font::Label, 12, ix + 22, yy, c::kTextFaint, "PRESS TO", 2.f);
      ui::Text(Font::Body, 16, ix + 22, yy + 18, c::kTextDim, ui::Fit(Font::Body, 16, what, iw - 44));
      if (r == Row::Game) {
        ui::ButtonGlyph(Button::Extra, ix + 22, yy + 62, 22);
        ui::Text(Font::Body, 15, ix + 52, yy + 52, c::kTextDim, "Choose an .iso file instead");
      }
    }
    // Where everything goes.
    ui::TextSpaced(Font::Label, 12, ix + 22, iy + ih - 70, c::kTextFaint, "EVERYTHING IS KEPT IN", 2.f);
    ui::Text(Font::Body, 14, ix + 22, iy + ih - 50, c::kTextDim,
             ui::Fit(Font::Body, 14, util::PathStr(layout_.dev ? layout_.source_dir : layout_.data_dir), iw - 44));
  }

  // Bottom: progress while working, otherwise the action button.
  if (busy) {
    DrawJobPanel(x - 10, 548, 1162, 104);
  } else {
    std::string label, sub;
    int left = 0;
    for (bool ready : {source_ready_ && libs_ready_, files_ready_, build_ready_})
      if (!ready) ++left;
    if (SetupComplete()) {
      label = "CONTINUE";
      sub = "Everything is ready. Back to the main menu.";
    } else if (!game_found_ && !files_ready_) {
      label = "CHOOSE YOUR GAME COPY";
      sub = "Pick the folder of your Games on Demand copy, or your .iso.";
    } else {
      label = last_error_.empty() ? "START SETUP" : "TRY AGAIN";
      if (!tools_.ok && tools_known_) ++left;
      sub = std::to_string(left) + (left == 1 ? " step" : " steps") + " left." +
            (build_ready_ ? "" : " The first build takes 30-90 minutes; you can leave it running.");
    }
    const bool sel = setup_sel_ == kRows;
    const float by = 552, bh = 64;
    ui::Chamfer(x, by, w, bh, 14, sel ? c::Rgba(20, 90, 130, 235) : c::Rgba(12, 30, 48, 220),
                sel ? c::kAccent : c::kPanelEdge, sel ? 2.f : 1.f);
    if (sel) ui::Rect(x, by + 14, 4, bh - 28, c::kAccent);
    ui::TextSpaced(Font::Heading, 24, x + 26, by + 8, sel ? c::kText : c::kTextDim, label, 1.6f);
    ui::Text(Font::Body, 15, x + 26, by + 39, sel ? c::Rgba(190, 230, 255) : c::kTextFaint,
             ui::Fit(Font::Body, 15, sub, w - 60));
    if (sel) ui::Triangle(x + w - 26, by + bh * 0.5f, 14, 0, c::kAccent);
  }

  // Actions
  if (busy) {
    if (Pressed(Action::Back)) CancelJob();
  } else {
    if (Pressed(Action::Back)) Go(Screen::Main);
    if (Pressed(Action::Start)) RunSetup();
    if (activate) {
      if (setup_sel_ == kRows) {
        if (SetupComplete())
          Go(Screen::Main);
        else
          RunSetup();
      } else {
        ActivateRow(Row(setup_sel_));
      }
    }
    if (Pressed(Action::Extra) && setup_sel_ == int(Row::Game)) PickGameIso();
  }
  if (Pressed(Action::Alt)) Go(Screen::Log);

  std::vector<ui::Prompt> prompts = {{Button::Move, "SELECT"}};
  if (busy) {
    prompts.push_back({Button::Alt, "BUILD LOG"});
    prompts.push_back({Button::Back, "STOP"});
  } else {
    prompts.push_back({Button::Accept, "OK"});
    prompts.push_back({Button::Alt, "BUILD LOG"});
    prompts.push_back({Button::Back, "BACK"});
  }
  ui::Footer(busy ? "You can leave this running. Closing the launcher stops it." : "", prompts);
}

// --- Settings ----------------------------------------------------------------------

void App::DrawSettings(float dt) {
  ui::Header("SETTINGS", HeaderStatus(), false);
  if (Pressed(Action::PrevTab) || Pressed(Action::NextTab)) settings_tab_ = 1 - settings_tab_;

  std::vector<SettingRow> rows;
  if (settings_tab_ == 0) {
    GameConfig& g = game_config_;
    auto save = [this] {
      if (!game_config_.Save()) Toast("Could not save the game's settings");
    };
    rows.push_back({"Fullscreen", "Start the game in fullscreen. Press F11 in the game to switch.", {"ON", "OFF"},
                    g.GetBool("fullscreen", true) ? 0 : 1, [&g, save](int i) { g.SetBool("fullscreen", i == 0); save(); }});
    rows.push_back({"Internal resolution",
                    "The resolution the game draws at. Higher looks sharper but needs a stronger graphics card. "
                    "The original is 1280 x 720.",
                    {"1280 x 720 (original)", "2560 x 1440", "3840 x 2160"},
                    std::clamp(g.GetInt("resolution_scale", 1), 1, 3) - 1,
                    [&g, save](int i) { g.SetInt("resolution_scale", i + 1); save(); }});
    static const char* kEffects[] = {"bilinear", "cas", "fsr"};
    int effect = 0;
    for (int i = 0; i < 3; ++i)
      if (g.GetString("present_effect", "bilinear") == kEffects[i]) effect = i;
    rows.push_back({"Upscaling filter", "How the picture is stretched to your screen. FSR and CAS look sharper.",
                    {"Bilinear", "AMD CAS (sharpen)", "AMD FSR"}, effect,
                    [&g, save](int i) { g.SetString("present_effect", kEffects[i]); save(); }});
    static const char* kAa[] = {"none", "fxaa", "fxaa_extreme"};
    int aa = 0;
    for (int i = 0; i < 3; ++i)
      if (g.GetString("swap_post_effect", "none") == kAa[i]) aa = i;
    rows.push_back({"Anti-aliasing", "Smooths jagged edges.", {"OFF", "FXAA", "FXAA (strong)"}, aa,
                    [&g, save](int i) { g.SetString("swap_post_effect", kAa[i]); save(); }});
    rows.push_back({"V-Sync", "Matches the frame rate to your screen to stop tearing.", {"ON", "OFF"},
                    g.GetBool("vsync", true) ? 0 : 1, [&g, save](int i) { g.SetBool("vsync", i == 0); save(); }});
    rows.push_back({"Letterbox", "Keeps the game's 16:9 shape on screens of other shapes.", {"ON", "OFF"},
                    g.GetBool("present_letterbox", true) ? 0 : 1,
                    [&g, save](int i) { g.SetBool("present_letterbox", i == 0); save(); }});
  } else {
    rows.push_back({"Launcher fullscreen", "Show this launcher fullscreen (F11 switches too).", {"ON", "OFF"},
                    settings_.GetBool("fullscreen", false) ? 0 : 1, [this](int i) { SetFullscreen(i == 0); }});
    static const int kJobs[] = {0, 1, 2, 4, 6, 8, 12, 16};
    int current = 0;
    for (int i = 0; i < 8; ++i)
      if (settings_.GetInt("build_jobs", 0) == kJobs[i]) current = i;
    std::vector<std::string> choices = {"Auto (" + std::to_string(steps::AutomaticJobs()) + ")"};
    for (int i = 1; i < 8; ++i) choices.push_back(std::to_string(kJobs[i]));
    rows.push_back({"Build jobs",
                    "How many parts are compiled at the same time. More is faster but needs more memory; "
                    "Auto picks a number that fits this PC.",
                    choices, current, [this](int i) {
                      settings_.SetInt("build_jobs", kJobs[i]);
                      settings_.Save();
                    }});
    rows.push_back({"Change game copy", "Choose a different folder or .iso as your copy of the game.", {}, 0, nullptr,
                    [this] { PickGameFolder(); }});
    rows.push_back({"Extract game files again", "Copies every game file from your copy again.", {}, 0, nullptr,
                    [this] { RunExtract(true); }});
    rows.push_back({"Build again", "Builds the PC version again. Only changed parts are recompiled.", {}, 0, nullptr,
                    [this] { RunBuild(true); }});
    rows.push_back({"View build log", "Everything the last setup, build or update printed.", {}, 0, nullptr,
                    [this] { Go(Screen::Log); }});
    rows.push_back({"Open the game's folder", "Opens the folder with the built game and its settings file.", {}, 0,
                    nullptr, [this] {
                      proc::OpenWithSystem(util::PathStr(util::IsDir(layout_.build_dir) ? layout_.build_dir : layout_.source_dir));
                    }});
  }

  const int n = int(rows.size());
  int& sel = settings_sel_[settings_tab_];
  sel = std::clamp(sel, 0, n - 1);
  if (Pressed(Action::Up)) sel = Wrap(sel - 1, n);
  if (Pressed(Action::Down)) sel = Wrap(sel + 1, n);

  // Tabs
  const char* tabs[] = {"GAME", "LAUNCHER"};
  const float tx = 108, ty = 122;
  ui::ButtonGlyph(Button::PrevTab, 64, ty + 20, 26);
  float tab_x = tx;
  for (int i = 0; i < 2; ++i) {
    const bool on = i == settings_tab_;
    const float tw = 170;
    ui::Chamfer(tab_x, ty, tw, 40, 8, on ? c::Rgba(18, 70, 104, 235) : c::Rgba(10, 24, 40, 200),
                on ? c::kAccent : c::kPanelEdge, 1.f);
    ui::TextSpaced(Font::Heading, 18, tab_x + tw * 0.5f - ui::Measure(Font::Heading, 18, tabs[i]).x * 0.5f -
                                         1.5f * float(std::char_traits<char>::length(tabs[i])) * 0.5f,
                   ty + 9, on ? c::kText : c::kTextDim, tabs[i], 1.5f);
    if (MouseIn(tab_x, ty, tw, 40) && Clicked()) settings_tab_ = i;
    tab_x += tw + 10;
  }
  ui::ButtonGlyph(Button::NextTab, tab_x + 4, ty + 20, 26);

  const float x = 64, y0 = 186, row_h = 58, w = 760;
  bool activate = Pressed(Action::Accept);
  for (int i = 0; i < n; ++i) {
    if (MouseIn(x, y0 + float(i) * row_h, w, row_h - 6)) {
      if (MouseMoved()) sel = i;
      if (Clicked()) {
        sel = i;
        activate = true;
      }
    }
  }
  ui::Panel(x - 10, y0 - 12, w + 20, float(std::max(n, 6)) * row_h + 14);
  anim_settings_ = demo_ ? float(sel) : Approach(anim_settings_, float(sel), dt);
  ui::MenuHighlight(x, y0 + anim_settings_ * row_h + 2, w, row_h - 8, 1.f);
  for (int i = 0; i < n; ++i) {
    const SettingRow& r = rows[size_t(i)];
    const float ry = y0 + float(i) * row_h;
    const bool on = i == sel;
    ui::Text(Font::Heading, 21, x + 24, ry + 13, on ? c::kText : c::kTextDim, r.label);
    const float right = x + w - 22;
    if (!r.choices.empty()) {
      const std::string& value = r.choices[size_t(std::clamp(r.current, 0, int(r.choices.size()) - 1))];
      const ImU32 vc = value == "OFF" ? c::kTextDim : (on ? c::kAccent : c::kText);
      const float vw = ui::Measure(Font::Heading, 19, value).x;
      ui::Text(Font::Heading, 19, right - 22, ry + 15, vc, value, 1.f);
      if (on) {
        ui::Triangle(right - 6, ry + 27, 12, 0, c::kAccent);
        ui::Triangle(right - 22 - vw - 16, ry + 27, 12, 2, c::kAccent);
      }
    } else {
      ui::Triangle(right - 6, ry + 27, 10, 0, on ? c::kAccent : c::kTextFaint);
    }
    if (i + 1 < n) ui::Line(x + 24, ry + row_h - 3, x + w - 22, ry + row_h - 3, ui::WithAlpha(c::kPanelEdge, 0.5f));
  }
  // Change values / run actions.
  SettingRow& cur = rows[size_t(sel)];
  if (!cur.choices.empty() && cur.set) {
    const int m = int(cur.choices.size());
    if (Pressed(Action::Left)) cur.set(Wrap(cur.current - 1, m));
    if (Pressed(Action::Right) || activate) cur.set(Wrap(cur.current + 1, m));
  } else if (activate && cur.action) {
    cur.action();
  }
  if (Pressed(Action::Extra) && settings_tab_ == 1 && sel == 2) PickGameIso();

  // Info panel
  const float ix = 848, iy = 174, iw = 368, ih = 362;
  ui::Panel(ix, iy, iw, ih, "INFO");
  std::string upper = cur.label;
  for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  ui::TextSpaced(Font::Heading, 19, ix + 22, iy + 24, c::kAccent, upper, 1.2f);
  ui::TextWrapped(Font::Body, 16, ix + 22, iy + 62, iw - 44, c::kText, cur.description);
  const char* note = settings_tab_ == 0
                         ? "Saved straight away to gundam_operation_troy.toml beside the game. Changes apply the next "
                           "time the game starts."
                         : "Launcher settings are saved straight away.";
  ui::TextWrapped(Font::Body, 14, ix + 22, iy + ih - 72, iw - 44, c::kTextFaint, note);

  if (Pressed(Action::Back)) Go(Screen::Main);
  ui::Footer("", {{Button::Move, cur.choices.empty() ? "SELECT" : "CHANGE"},
                  {Button::PrevTab, "TAB"},
                  {Button::Accept, cur.choices.empty() ? "OK" : "NEXT"},
                  {Button::Back, "BACK"}});
}

// --- Build log ---------------------------------------------------------------------

void App::DrawLog(float /*dt*/) {
  ui::Header("BUILD LOG", HeaderStatus(), false);
  const float x = 64, y = 126, w = 1152, h = 520;
  ui::Panel(x, y, w, h);
  const float line_h = 18.5f;
  const int visible = int((h - 26) / line_h);
  const size_t count = log_.Count();
  const double max_top = std::max(0.0, double(count) - double(visible));
  double scroll = log_scroll_;
  if (Pressed(Action::Up)) { scroll -= 1; log_follow_ = false; }
  if (Pressed(Action::Down)) scroll += 1;
  if (Pressed(Action::PrevTab) || Pressed(Action::Left)) { scroll -= visible - 1; log_follow_ = false; }
  if (Pressed(Action::NextTab) || Pressed(Action::Right)) scroll += visible - 1;
  if (wheel_ != 0.f) {
    scroll -= double(wheel_) * 3.0;
    if (wheel_ > 0) log_follow_ = false;
  }
  if (scroll >= max_top) log_follow_ = true;
  if (log_follow_) scroll = max_top;
  log_scroll_ = std::clamp(scroll, 0.0, max_top);

  const size_t top = size_t(log_scroll_);
  auto lines = log_.Range(top, size_t(visible));
  ui::PushClip(x + 8, y + 8, w - 30, h - 16);
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    ImU32 color = c::kTextDim;
    if (util::ContainsI(line, "error") || util::StartsWithI(line, "FAILED")) color = c::Rgba(255, 140, 140);
    else if (util::ContainsI(line, "warning")) color = c::kWarn;
    else if (line.rfind("> ", 0) == 0 || line.rfind("===", 0) == 0 || line.rfind("---", 0) == 0) color = c::kAccent;
    else if (!line.empty() && line[0] == '[') color = c::kTextFaint;
    ui::Text(Font::Mono, 14, x + 20, y + 14 + float(i) * line_h, color, ui::Fit(Font::Mono, 14, line, w - 60));
  }
  ui::PopClip();
  if (count == 0) ui::Text(Font::Body, 18, x + w * 0.5f, y + h * 0.5f - 10, c::kTextFaint, "Nothing here yet.", 0.5f);
  // Scrollbar
  if (count > size_t(visible)) {
    const float track_h = h - 40;
    const float thumb_h = std::max(30.f, track_h * float(visible) / float(count));
    const float thumb_y = y + 20 + (track_h - thumb_h) * float(log_scroll_ / std::max(1.0, max_top));
    ui::Rect(x + w - 16, y + 20, 3, track_h, c::Rgba(24, 42, 62));
    ui::Rect(x + w - 17, thumb_y, 5, thumb_h, c::kAccentDim);
  }
  if (Pressed(Action::Accept)) proc::OpenWithSystem(util::PathStr(log_.FilePath()));
  if (Pressed(Action::Back)) Go(log_return_ == Screen::Log ? Screen::Main : log_return_);
  ui::Footer(util::PathStr(log_.FilePath()),
             {{Button::Move, "SCROLL"}, {Button::PrevTab, "PAGE"}, {Button::Accept, "OPEN FILE"}, {Button::Back, "BACK"}});
}

// --- Game running ------------------------------------------------------------------

void App::DrawRunning(float /*dt*/) {
  ui::Header("IN GAME", HeaderStatus(), false);
  const float w = 620, h = 200, x = (ui::kCanvasW - w) * 0.5f, y = 240;
  ui::Panel(x, y, w, h);
  ui::Spinner(x + 50, y + 64, 16, c::kAccent);
  ui::TextSpaced(Font::Heading, 26, x + 84, y + 46, c::kText, "GAME RUNNING", 2.f);
  ui::TextWrapped(Font::Body, 17, x + 84, y + 92, w - 120, c::kTextDim,
                  "Gundam Operation Troy is running in its own window. Close the game to come back here.");
  ui::Footer("", {});
}

// --- Dialog and toast --------------------------------------------------------------

void App::DrawDialog(float /*dt*/) {
  if (!dialog_) return;
  Dialog& d = *dialog_;
  const int n = std::max(1, int(d.options.size()));
  if (Pressed(Action::Left) || Pressed(Action::Up)) d.selected = Wrap(d.selected - 1, n);
  if (Pressed(Action::Right) || Pressed(Action::Down)) d.selected = Wrap(d.selected + 1, n);
  int choice = -2;
  if (Pressed(Action::Accept)) choice = d.selected;
  if (Pressed(Action::Back)) choice = d.back_choice >= 0 ? d.back_choice : -1;

  ui::Rect(-2000, -2000, 5280, 4720, c::Rgba(0, 0, 0, 205));
  const float w = 660;
  const float msg_h = ui::WrappedHeight(Font::Body, 18, w - 72, d.message);
  const float h = 74 + msg_h + 104;
  const float x = (ui::kCanvasW - w) * 0.5f, y = (ui::kCanvasH - h) * 0.5f;
  ui::Chamfer(x, y, w, h, 18, c::Rgba(7, 15, 27, 248), ui::WithAlpha(d.accent, 0.85f), 1.5f);
  ui::Rect(x + 18, y, 140, 3, d.accent);
  ui::Brackets(x - 6, y - 6, w + 12, h + 12, 16, ui::WithAlpha(d.accent, 0.5f), 2.f);
  ui::TextSpaced(Font::Heading, 24, x + 36, y + 26, d.accent, d.title, 1.6f);
  ui::TextWrapped(Font::Body, 18, x + 36, y + 70, w - 72, c::kText, d.message);

  // Buttons, right-aligned.
  const float bh = 46, by = y + h - bh - 26;
  std::vector<float> widths;
  float total = 0;
  for (const auto& o : d.options) {
    widths.push_back(std::max(150.f, ui::Measure(Font::Heading, 18, o).x + 56.f));
    total += widths.back() + 14;
  }
  float bx = x + w - 36 - (total - 14);
  for (int i = 0; i < int(d.options.size()); ++i) {
    const bool sel = i == d.selected;
    const float bw = widths[size_t(i)];
    if (MouseIn(bx, by, bw, bh)) {
      if (MouseMoved()) d.selected = i;
      if (Clicked()) choice = i;
    }
    ui::Chamfer(bx, by, bw, bh, 10, sel ? ui::WithAlpha(d.accent, 0.9f) : c::Rgba(14, 30, 48, 230),
                sel ? d.accent : c::kPanelEdge, 1.f);
    ui::Text(Font::Heading, 18, bx + bw * 0.5f, by + 11, sel ? c::Rgba(4, 12, 22) : c::kTextDim,
             d.options[size_t(i)], 0.5f);
    bx += bw + 14;
  }
  if (choice != -2) {
    auto callback = std::move(d.on_choose);
    dialog_.reset();
    if (callback && choice >= 0) callback(choice);
  }
}

void App::DrawToast() {
  if (toast_.empty() || now_ > toast_until_) return;
  const float a = float(std::min(1.0, (toast_until_ - now_) / 0.4));
  const float tw = ui::Measure(Font::Body, 17, toast_).x;
  const float w = tw + 64, h = 40, x = (ui::kCanvasW - w) * 0.5f, y = 112;
  ui::Chamfer(x, y, w, h, 10, ui::WithAlpha(c::Rgba(8, 40, 60), 0.95f * a), ui::WithAlpha(c::kAccent, a), 1.f);
  ui::Circle(x + 22, y + h * 0.5f, 4, ui::WithAlpha(c::kAccent, a));
  ui::Text(Font::Body, 17, x + 38, y + 9, ui::WithAlpha(c::kText, a), toast_);
}

// --- Screenshot scenes -------------------------------------------------------------

int App::DemoCount() { return 9; }

const char* App::DemoName(int index) {
  static const char* kNames[] = {"01_main_menu",       "02_setup_first_run", "03_setup_building",
                                 "04_settings_game",   "05_settings_launcher", "06_main_first_time",
                                 "07_build_log",       "08_install_tools",   "09_setup_error"};
  return kNames[std::clamp(index, 0, DemoCount() - 1)];
}

void App::LoadDemo(int index) {
  demo_ = true;
  dialog_.reset();
  toast_.clear();
  last_error_.clear();
  last_error_lines_.clear();
  job_.running = false;
  using_gamepad_ = true;
  gamepad_type_ = SDL_GAMEPAD_TYPE_XBOXONE;

  const std::string repo = "E:\\Games\\Mobile-Suit-Gundam-Operation-Troy";
  layout_.dev = true;
  layout_.exe_dir = layout_.source_dir = util::U8Path(repo);
  layout_.data_dir = util::U8Path(repo + "\\out");
  layout_.saves_dir = util::U8Path(repo + "\\saves");
  layout_.game_dir = util::U8Path(repo + "\\game");
  layout_.build_dir = util::U8Path(repo + "\\out\\build\\win-amd64-release");

  tools_ = toolchain::Info{};
  tools_.ok = true;
  tools_.vs_name = "Visual Studio Build Tools 2022";
  tools_.clang_version = "19.1.5";
  tools_.cmake_version = "3.31.6";
  tools_.ninja_version = "1.12.1";
  tools_known_ = true;

  game_ = finder::Candidate{};
  game_.path = util::U8Path(
      "E:\\Games\\Emulators\\Microsoft Xbox 360 Xenia Manager\\IMAGES\\Games On Demand\\Gundam Operation Troy English");
  game_.kind = gdf::Kind::God;
  game_.title_id = gdf::kOperationTroyTitleId;
  game_.file_count = 654;
  game_.total_bytes = 1381000000ull;
  game_found_ = true;
  source_ready_ = libs_ready_ = true;
  source_version_ = "main @ 3138bfc";
  files_ready_ = true;
  files_count_ = 654;
  files_bytes_ = 1381000000ull;
  build_ready_ = true;
  built_at_ = util::NowUnix() - 3 * 3600;
  screen_ = Screen::Main;
  main_sel_ = 0;
  setup_sel_ = 0;

  log_.Clear();
  const char* sample[] = {
      "=== SETUP (Operation Troy launcher 0.1.0) ===",
      "--- Building the PC version ---",
      "Compiler: clang 19.1.5, CMake 3.31.6, 8 parallel jobs",
      "> cmake --preset win-amd64-release",
      "-- Using ReXGlue SDK from source tree: E:/Games/Mobile-Suit-Gundam-Operation-Troy/thirdparty/rexglue-sdk",
      "-- Configuring done (14.2s)",
      "-- Generating done (3.1s)",
      "> cmake --build out/build/win-amd64-release --target gundam_operation_troy_codegen -j 8",
      "[211/212] Generating recompiled code for gundam_operation_troy",
      "[212/212] Linking ...",
      "> cmake --build out/build/win-amd64-release -j 8",
      "[806/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.118.cpp.obj",
      "[807/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.119.cpp.obj",
      "[808/2210] Building CXX object thirdparty/rexglue-sdk/src/graphics/CMakeFiles/rex_graphics.dir/pipeline/texture/cache.cpp.obj",
      "[809/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.120.cpp.obj",
      "[810/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.121.cpp.obj",
      "[811/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.122.cpp.obj",
      "[812/2210] Building CXX object CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.123.cpp.obj",
  };
  for (const char* line : sample) log_.Add(line);

  switch (index) {
    case 0:
      break;
    case 1:
      files_ready_ = build_ready_ = false;
      screen_ = Screen::Setup;
      setup_sel_ = int(Row::Count);
      break;
    case 2: {
      build_ready_ = false;
      screen_ = Screen::Setup;
      setup_sel_ = int(Row::Build);
      job_.running = true;
      std::lock_guard lock(job_.mu);
      job_.title = "SETUP";
      job_.stage = "Compiling  812 / 2210";
      job_.progress = 0.14f + 0.86f * 812.f / 2210.f;
      job_.step_index = 2;
      job_.step_count = 2;
      job_.row = Row::Build;
      job_.done = 812;
      job_.total = 2210;
      job_.started = now_ - 1520;
      job_.eta = 31 * 60;
      break;
    }
    case 3:
      screen_ = Screen::Settings;
      settings_tab_ = 0;
      settings_sel_[0] = 1;
      game_config_.Load(util::U8Path("/nonexistent/demo.toml"));
      game_config_.SetInt("resolution_scale", 2);
      game_config_.SetString("present_effect", "fsr");
      break;
    case 4:
      screen_ = Screen::Settings;
      settings_tab_ = 1;
      settings_sel_[1] = 1;
      break;
    case 5:
      game_found_ = false;
      files_ready_ = build_ready_ = false;
      break;
    case 6:
      screen_ = Screen::Log;
      log_follow_ = true;
      break;
    case 7: {
      tools_.ok = false;
      tools_.missing = {"clang 18 or newer", "CMake 3.25 or newer"};
      tools_.can_install = true;
      files_ready_ = build_ready_ = false;
      screen_ = Screen::Setup;
      setup_sel_ = int(Row::Tools);
      ConfirmInstallTools([] {});
      break;
    }
    case 8:
      build_ready_ = false;
      screen_ = Screen::Setup;
      setup_sel_ = int(Row::Count);
      last_error_title_ = "SETUP";
      last_error_ = "Compiling failed. See the log for the first error.";
      last_error_lines_ = {
          "FAILED: CMakeFiles/gundam_operation_troy_recomp.dir/generated/default/gundam_operation_troy_recomp.123.cpp.obj",
          "gundam_operation_troy_recomp.123.cpp(4512,7): error: use of undeclared identifier 'ctx'"};
      break;
    default:
      break;
  }
  anim_main_ = float(main_sel_);
  anim_setup_ = float(setup_sel_);
  anim_settings_ = float(settings_sel_[settings_tab_]);
  screen_fade_ = 1.f;
}
