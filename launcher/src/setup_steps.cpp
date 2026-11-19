#include "setup_steps.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <system_error>

#include <SDL3/SDL.h>

#include "gdf.h"
#include "subprocess.h"

namespace {

const std::string kRepoUrl = std::string("https://github.com/") + OPTROY_REPO_OWNER + "/" + OPTROY_REPO_NAME;
constexpr const char* kSourceArchive = "optroy-source.zip";
constexpr const char* kExtractedMarker = ".optroy-extracted";
constexpr const char* kBuiltMarker = ".optroy-built";
#ifdef _WIN32
constexpr const char* kNullDevice = "NUL";
#else
constexpr const char* kNullDevice = "/dev/null";
#endif

void Report(JobContext& ctx, const std::string& stage, float progress) {
  if (ctx.stage) ctx.stage(stage);
  if (ctx.progress) ctx.progress(progress);
  if (ctx.counter) ctx.counter(0, 0);
}

bool LooksLikeError(std::string_view line) {
  return util::ContainsI(line, "error:") || util::ContainsI(line, "error LNK") || util::StartsWithI(line, "FAILED:") ||
         util::ContainsI(line, "CMake Error") || util::ContainsI(line, "fatal error") || util::ContainsI(line, "fatal:");
}

// Runs a command with its command line and output in the log.
int RunLogged(JobContext& ctx, const proc::Command& cmd, const proc::LineFn& on_line = nullptr) {
  ctx.log->Add("> " + (cmd.raw_cmdline.empty() ? proc::JoinArgs(cmd.args) : cmd.raw_cmdline));
  std::string err;
  const int code = proc::Run(
      cmd,
      [&](std::string_view line) {
        ctx.log->Add(line);
        if (LooksLikeError(line) && ctx.error_lines.size() < 40) ctx.error_lines.emplace_back(line);
        if (on_line) on_line(line);
      },
      ctx.cancel, &err);
  if (code == proc::kStartFailed) {
    ctx.log->Add(err);
    ctx.error = err;
  } else if (code == proc::kCancelled) {
    ctx.log->Add("(stopped)");
    ctx.error = "Stopped.";
  } else if (code != 0) {
    ctx.log->Printf("(exit code %d)", code);
  }
  return code;
}

// "[12/345] Building CXX object ..." -> 12, 345
bool NinjaProgress(std::string_view line, int& done, int& total) {
  if (line.size() < 5 || line[0] != '[') return false;
  const size_t slash = line.find('/'), close = line.find(']');
  if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) return false;
  done = std::atoi(std::string(line.substr(1, slash - 1)).c_str());
  total = std::atoi(std::string(line.substr(slash + 1, close - slash - 1)).c_str());
  return total > 0 && done >= 0 && done <= total;
}

// The last "45%" / "45.3%" in a line (curl, git).
bool PercentIn(std::string_view line, float& pct) {
  const size_t p = line.rfind('%');
  if (p == std::string_view::npos || p == 0) return false;
  size_t b = p;
  while (b > 0 && (std::isdigit(static_cast<unsigned char>(line[b - 1])) || line[b - 1] == '.')) --b;
  if (b == p) return false;
  pct = std::strtof(std::string(line.substr(b, p - b)).c_str(), nullptr);
  return pct >= 0.f && pct <= 100.f;
}

proc::Command Cmd(std::vector<std::string> args, const fs::path& cwd = {}, const proc::Env* env = nullptr) {
  proc::Command c;
  c.args = std::move(args);
  if (!cwd.empty()) c.cwd = util::PathStr(cwd);
  c.env = env;
  return c;
}

bool Download(JobContext& ctx, const toolchain::Info& tools, const std::string& url, const fs::path& dest,
              const std::string& what, float from, float to) {
  if (tools.curl.empty()) {
    ctx.error = "curl was not found, so " + what + " cannot be downloaded.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  Report(ctx, "Downloading " + what, from);
  const int code = RunLogged(ctx, Cmd({tools.curl, "-L", "--fail", "--progress-bar", "-o", util::PathStr(dest), url}),
                             [&](std::string_view line) {
                               float pct = 0;
                               if (PercentIn(line, pct) && ctx.progress) ctx.progress(from + (to - from) * pct / 100.f);
                             });
  if (code != 0) {
    if (ctx.error.empty()) ctx.error = "Downloading " + what + " failed. Check your internet connection.";
    return false;
  }
  return true;
}

bool Unpack(JobContext& ctx, const toolchain::Info& tools, const fs::path& archive, const fs::path& dest) {
  if (tools.tar.empty()) {
    ctx.error = "tar was not found, so the download cannot be unpacked.";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dest, ec);
  if (RunLogged(ctx, Cmd({tools.tar, "-xf", util::PathStr(archive), "-C", util::PathStr(dest)})) != 0) {
    if (ctx.error.empty()) ctx.error = "Unpacking " + util::PathStr(archive.filename()) + " failed.";
    return false;
  }
  return true;
}

proc::Env GitEnv();
std::vector<std::string> Git(const toolchain::Info& tools, std::vector<std::string> args);

bool GitHelperLibs(JobContext& ctx, const toolchain::Info& tools) {
  const Layout& l = ctx.layout;
  if (steps::HelperLibsReady(l)) return true;
  if (tools.git.empty() || !util::Exists(l.source_dir / ".git")) {
    ctx.error = "The SDK's helper libraries are missing and git is not available to download them.";
    return false;
  }
  Report(ctx, "Downloading the SDK's helper libraries (about 400 MB)", -1.f);
  const proc::Env env = GitEnv();
  if (RunLogged(ctx, Cmd(Git(tools, {"submodule", "update", "--init", "--depth", "1", "--jobs", "8"}), l.source_dir, &env)) != 0) {
    if (ctx.error.empty()) ctx.error = "Downloading the helper libraries failed. Check your internet connection.";
    return false;
  }
  return true;
}

// git never waits for a password prompt nobody can see, and copes with deep paths.
proc::Env GitEnv() {
  proc::Env env = proc::CurrentEnvironment();
  env["GIT_TERMINAL_PROMPT"] = "0";
  env["GCM_INTERACTIVE"] = "never";
  return env;
}

std::vector<std::string> Git(const toolchain::Info& tools, std::vector<std::string> args) {
  std::vector<std::string> full = {tools.git, "-c", "core.longpaths=true", "-c", "advice.detachedHead=false"};
  full.insert(full.end(), args.begin(), args.end());
  return full;
}

std::string GitHead(JobContext& ctx, const toolchain::Info& tools) {
  std::string out;
  proc::Capture(Cmd({tools.git, "rev-parse", "--short", "HEAD"}, ctx.layout.source_dir), &out, nullptr);
  return util::Trim(out);
}

}  // namespace

Layout DetectLayout(const fs::path& exe_dir) {
  Layout l;
  l.exe_dir = exe_dir;
  fs::path dir = exe_dir;
  for (int i = 0; i < 6 && !dir.empty(); ++i) {
    if (util::IsFile(dir / "gundam_operation_troy_manifest.toml") && util::IsDir(dir / "thirdparty")) {
      l.dev = true;
      l.source_dir = dir;
      break;
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) break;
    dir = dir.parent_path();
  }
  if (l.dev) {
    l.data_dir = l.source_dir / "out";
    l.saves_dir = l.source_dir / "saves";
  } else {
    l.data_dir = exe_dir / "RecompData";
    l.source_dir = l.data_dir / "source";
    l.saves_dir = exe_dir / "Saves";
  }
  l.game_dir = l.source_dir / "game";
  l.build_dir = l.source_dir / "out" / "build" / toolchain::PresetName();
  l.tools_dir = l.data_dir / "tools";
  l.log_path = l.data_dir / "build.log";
  l.settings_path = l.data_dir / "launcher.ini";
  return l;
}

namespace steps {

bool SourceReady(const Layout& l) {
  return util::IsFile(l.source_dir / "gundam_operation_troy_manifest.toml") &&
         util::IsFile(l.source_dir / "thirdparty" / "rexglue-sdk" / "CMakeLists.txt");
}

bool HelperLibsReady(const Layout& l) {
  const auto libs = l.source_dir / "thirdparty" / "rexglue-sdk" / "thirdparty";
  // rapidjson came later (online play), so an older copy can have fmt but not rapidjson.
  return util::IsFile(libs / "fmt" / "CMakeLists.txt") &&
         util::IsFile(libs / "rapidjson" / "include" / "rapidjson" / "document.h");
}

std::string SourceVersion(const Layout& l) {
  std::string v;
  if (util::ReadTextFile(l.source_dir / "VERSION", v)) return util::Trim(v);
  return {};
}

bool GameFilesReady(const Layout& l, size_t* files, uint64_t* bytes) {
  if (!util::IsFile(l.game_dir / "default.xex")) return false;
  std::string marker;
  if (util::ReadTextFile(l.game_dir / kExtractedMarker, marker)) {
    for (const auto& line : util::SplitLines(marker)) {
      if (files && line.rfind("files=", 0) == 0) *files = std::strtoull(line.c_str() + 6, nullptr, 10);
      if (bytes && line.rfind("bytes=", 0) == 0) *bytes = std::strtoull(line.c_str() + 6, nullptr, 10);
    }
    return true;
  }
  return util::IsDir(l.game_dir / "Media");  // extracted earlier by build.bat
}

bool BuildReady(const Layout& l, int64_t* built_at) {
  if (!util::IsFile(l.GameExe())) return false;
  if (built_at) *built_at = util::FileTimeUnix(l.GameExe());
  return true;
}

int AutomaticJobs() {
  const int cores = std::max(1, SDL_GetNumLogicalCPUCores());
  const int ram_gb = std::max(1, SDL_GetSystemRAM() / 1024);
  // The converted game code is very large per file; allow about 2 GB of memory per job.
  return std::clamp(std::min(cores, ram_gb / 2), 1, 32);
}

bool GetSource(JobContext& ctx, const toolchain::Info& tools) {
  const Layout& l = ctx.layout;
  std::error_code ec;
  if (!SourceReady(l)) {
    if (l.dev) {
      ctx.error = "The repository copy at " + util::PathStr(l.source_dir) + " is incomplete.";
      return false;
    }
    const bool empty = !util::Exists(l.source_dir) || fs::is_empty(l.source_dir, ec);
    const fs::path target = empty ? l.source_dir : l.data_dir / "source.new";
    if (!empty) fs::remove_all(target, ec);
    fs::create_directories(target.parent_path(), ec);

    if (!tools.git.empty()) {
      Report(ctx, "Downloading the recomp source", 0.f);
      const proc::Env env = GitEnv();
      const int code = RunLogged(ctx,
                                 Cmd(Git(tools, {"clone", "--depth", "1", "--progress", kRepoUrl + ".git",
                                                 util::PathStr(target)}),
                                     {}, &env),
                                 [&](std::string_view line) {
                                   float pct = 0;
                                   if (util::ContainsI(line, "Receiving objects") && PercentIn(line, pct) && ctx.progress)
                                     ctx.progress(pct / 100.f);
                                 });
      if (code != 0) {
        if (ctx.error.empty()) ctx.error = "Downloading the recomp source failed. Check your internet connection.";
        return false;
      }
    } else {
      const fs::path archive = l.data_dir / kSourceArchive;
      if (!Download(ctx, tools, kRepoUrl + "/releases/latest/download/" + kSourceArchive, archive,
                    "the recomp source (about 400 MB)", 0.f, 0.9f))
        return false;
      Report(ctx, "Unpacking the recomp source", -1.f);
      if (!Unpack(ctx, tools, archive, target)) return false;
      fs::remove(archive, ec);
    }

    if (!empty) {
      // Keep the extracted game and any build output from the old folder.
      for (const char* keep : {"game", "out"}) {
        if (util::IsDir(l.source_dir / keep) && !util::Exists(target / keep))
          fs::rename(l.source_dir / keep, target / keep, ec);
      }
      fs::remove_all(l.source_dir, ec);
      fs::rename(target, l.source_dir, ec);
      if (ec) {
        ctx.error = "Could not move the new source into place: " + ec.message();
        return false;
      }
    }
    if (!SourceReady(l)) {
      ctx.error = "The downloaded source is incomplete.";
      return false;
    }
  }
  return GitHelperLibs(ctx, tools);
}

// Plain words for the Visual Studio installer's exit codes (Microsoft's documented list).
std::string InstallerCodeText(int code) {
  switch (code) {
    case 740: return "it needs administrator permission";
    case 1001: return "the Visual Studio Installer is already open; close it and try again";
    case 1003: return "Visual Studio is in use; close it and try again";
    case 1602:
    case 5004: return "it was cancelled";
    case 1618: return "another installation is running; wait for it to finish";
    case 5003: return "it could not download its files; check the internet connection";
    case 5007: return "Microsoft's installer says this PC does not meet a requirement (often low disk space on C:, or the installer needs updating first)";
    case 8006: return "Visual Studio programs are running; close them and try again";
    case 8010: return "this version of Windows is not supported";
    default: return "";
  }
}

bool InstallerCancelled(int code) { return code == 1602 || code == 5004; }

std::string InstallerFailure(int code, const std::string& err) {
  if (code == proc::kStartFailed) return "The installer did not start: " + err;
  std::string s = "The installer stopped (code " + std::to_string(code) + ")";
  const std::string why = InstallerCodeText(code);
  return why.empty() ? s + "." : s + ": " + why + ".";
}

#ifdef _WIN32
int RunInstaller(JobContext& ctx, const std::string& exe, const std::string& params, std::string* err) {
  ctx.log->Add("> " + exe + " " + params);
  const int code = proc::RunShell(exe, params, ctx.cancel, err);
  if (code == proc::kStartFailed)
    ctx.log->Add("The installer did not start: " + *err);
  else if (code != proc::kCancelled)
    ctx.log->Add("Installer exit code " + std::to_string(code) +
                 (InstallerCodeText(code).empty() ? "" : " (" + InstallerCodeText(code) + ")"));
  return code;
}
#endif

bool InstallBuildTools(JobContext& ctx, const toolchain::Info& tools) {
#ifdef _WIN32
  ctx.log->Add("Found: " + (tools.vs_path.empty() ? std::string("no Visual Studio with C++ tools")
                                                  : tools.vs_name + " (" + tools.vs_path + ")"));
  ctx.log->Add(tools.Summary());
  std::string err;

  // An existing Visual Studio 2022 or newer gets the missing parts added. Anything
  // older (or nothing) gets a fresh Build Tools 2022 beside it.
  if (!tools.vs_path.empty() && tools.vs_year >= 2022) {
    const char* pf86 = SDL_getenv("ProgramFiles(x86)");
    const std::string setup =
        util::PathStr(util::U8Path(pf86 ? pf86 : "") / "Microsoft Visual Studio" / "Installer" / "setup.exe");
    const std::string where = "--installPath " + proc::QuoteArg(tools.vs_path);

    // A clang that is too old means this Visual Studio is out of date, and Microsoft's
    // installer often refuses to add parts to an out-of-date copy. Update it first.
    if (!tools.old_clang.empty()) {
      Report(ctx, "Updating Visual Studio (it may ask for permission)", -1.f);
      ctx.log->Add("clang " + tools.old_clang + " is too old, so Visual Studio is updated first.");
      const int code = RunInstaller(ctx, setup, "update " + where + " --passive --norestart", &err);
      if (code == proc::kCancelled || InstallerCancelled(code)) {
        ctx.error = "The update was cancelled.";
        return false;
      }
      // If the update fails, the steps below still run and show Microsoft's own window.
    }

    Report(ctx, "Microsoft's installer is running (it may ask for permission; about 6 GB)", -1.f);
    const std::string params = "modify " + where + " " + toolchain::InstallerComponents();
    int code = RunInstaller(ctx, setup, params + " --passive --norestart", &err);
    if (code == 0 || code == 3010) {
      ctx.log->Add(code == 3010 ? "Build Tools installed (Windows asks for a restart)." : "Build Tools installed.");
      return true;
    }
    if (code == proc::kCancelled || code == proc::kStartFailed || InstallerCancelled(code)) {
      ctx.error = code == proc::kCancelled ? "Cancelled." : InstallerFailure(code, err);
      return false;
    }

    // The quiet install failed. Open Microsoft's installer window with the same parts
    // ticked, so the player can see its reason (disk space, an update it needs) and fix it there.
    const std::string first_failure = InstallerFailure(code, err);
    ctx.log->Add(first_failure);
    ctx.log->Add("Opening Microsoft's installer window so you can see what it needs.");
    Report(ctx, "Finish in Microsoft's installer window, then close it", -1.f);
    code = RunInstaller(ctx, setup, params + " --norestart", &err);
    if (code == 0 || code == 3010) {
      ctx.log->Add(code == 3010 ? "Build Tools installed (Windows asks for a restart)." : "Build Tools installed.");
      return true;
    }
    if (code == proc::kCancelled) {
      ctx.error = "Cancelled.";
      return false;
    }
    // Closing the window can give a non-zero code even when the parts went in, so let
    // the check that follows decide. Keep the first reason in case they are still missing.
    ctx.log->Add("Checking the build tools again.");
    ctx.error = first_failure;
    return true;
  }

  const fs::path bootstrapper = ctx.layout.tools_dir / "vs_BuildTools.exe";
  if (!Download(ctx, tools, "https://aka.ms/vs/17/release/vs_BuildTools.exe", bootstrapper,
                "Microsoft's Build Tools installer", 0.f, 0.05f))
    return false;
  Report(ctx, "Microsoft's installer is running (it may ask for permission; about 6 GB)", -1.f);
  const std::string params = toolchain::InstallerComponents() + " --wait --norestart";
  int code = RunInstaller(ctx, util::PathStr(bootstrapper), params + " --passive", &err);
  if (code == 0 || code == 3010) {
    ctx.log->Add(code == 3010 ? "Build Tools installed (Windows asks for a restart)." : "Build Tools installed.");
    return true;
  }
  if (code == proc::kCancelled || code == proc::kStartFailed || InstallerCancelled(code)) {
    ctx.error = code == proc::kCancelled ? "Cancelled." : InstallerFailure(code, err);
    return false;
  }
  const std::string first_failure = InstallerFailure(code, err);
  ctx.log->Add(first_failure);
  ctx.log->Add("Opening Microsoft's installer window so you can see what it needs.");
  Report(ctx, "Finish in Microsoft's installer window, then close it", -1.f);
  code = RunInstaller(ctx, util::PathStr(bootstrapper), params, &err);
  if (code == 0 || code == 3010) {
    ctx.log->Add(code == 3010 ? "Build Tools installed (Windows asks for a restart)." : "Build Tools installed.");
    return true;
  }
  if (code == proc::kCancelled) {
    ctx.error = "Cancelled.";
    return false;
  }
  ctx.log->Add("Checking the build tools again.");
  ctx.error = first_failure;
  return true;
#else
  ctx.error = "Install clang 18 or newer, CMake 3.25 or newer and Ninja with your system's package manager.";
  return false;
#endif
}

bool ExtractGame(JobContext& ctx, const fs::path& game_copy) {
  const Layout& l = ctx.layout;
  Report(ctx, "Reading your copy of the game", -1.f);
  ctx.log->Add("Game copy: " + util::PathStr(game_copy));
  gdf::Source src;
  std::string err;
  if (!gdf::Open(game_copy, src, err)) {
    ctx.error = "Your copy of the game could not be opened: " + err;
    ctx.log->Add(ctx.error);
    return false;
  }
  std::vector<gdf::Entry> entries;
  if (!gdf::List(src, entries, err) && entries.empty()) {
    ctx.error = "Your copy of the game could not be read: " + err;
    ctx.log->Add(ctx.error);
    return false;
  }
  const uint32_t title = gdf::ReadTitleId(src, entries);
  if (title != gdf::kOperationTroyTitleId) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "This is not Gundam Operation Troy (its title ID is %08X, not 424107D6).", title);
    ctx.error = buf;
    ctx.log->Add(ctx.error);
    return false;
  }
  std::error_code ec;
  if (src.kind == gdf::Kind::Folder && fs::equivalent(src.path, l.game_dir, ec)) {
    ctx.log->Add("The game files are already in place.");
  } else {
    fs::remove(l.game_dir / kExtractedMarker, ec);
    Report(ctx, "Extracting the game files", 0.f);
    gdf::Progress done;
    const bool ok = gdf::Extract(
        src, entries, l.game_dir, true,
        [&](const gdf::Progress& p) {
          if (ctx.progress && p.total_bytes) ctx.progress(float(double(p.done_bytes) / double(p.total_bytes)));
          if (ctx.counter) ctx.counter(int(p.done_bytes >> 20), int(p.total_bytes >> 20));
        },
        ctx.cancel, err, &done);
    if (!ok) {
      ctx.error = err == "cancelled" ? "Stopped." : "Extracting failed: " + err;
      ctx.log->Add(ctx.error);
      return false;
    }
    ctx.log->Printf("Extracted %zu files (%s) into %s", done.done_files, util::FormatBytes(done.done_bytes).c_str(),
                    util::PathStr(l.game_dir).c_str());
    util::WriteTextFile(l.game_dir / kExtractedMarker,
                        "files=" + std::to_string(done.done_files) + "\nbytes=" + std::to_string(done.done_bytes) +
                            "\nfrom=" + util::PathStr(game_copy) + "\nwhen=" + std::to_string(util::NowUnix()) + "\n");
  }
  return true;
}

bool BuildGame(JobContext& ctx, toolchain::Info& tools) {
  const Layout& l = ctx.layout;
  if (!tools.ok) {
    ctx.error = "Build tools are missing. " + tools.Summary();
    return false;
  }
  std::error_code ec;
#ifdef _WIN32
  if (tools.ninja.empty()) {
    const fs::path zip = l.tools_dir / "ninja-win.zip";
    if (!Download(ctx, tools, "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip", zip,
                  "Ninja (the build runner)", 0.f, 0.01f) ||
        !Unpack(ctx, tools, zip, l.tools_dir / "ninja"))
      return false;
    fs::remove(zip, ec);
    tools.ninja = util::PathStr(l.tools_dir / "ninja" / "ninja.exe");
  }
#endif
  if (tools.ninja.empty()) {
    ctx.error = "Ninja (the build runner) was not found.";
    return false;
  }

  Report(ctx, "Loading the compiler environment", -1.f);
  proc::Env env;
  std::string err;
  if (!toolchain::BuildEnvironment(tools, env, err)) {
    ctx.error = err;
    ctx.log->Add(err);
    return false;
  }
  const std::string preset = toolchain::PresetName();
  const std::string build_dir = util::PathStr(l.build_dir);
  const int jobs = ctx.build_jobs > 0 ? ctx.build_jobs : steps::AutomaticJobs();
  ctx.log->Printf("Compiler: clang %s, CMake %s, %d parallel jobs", tools.clang_version.c_str(),
                  tools.cmake_version.c_str(), jobs);

  auto configure = [&](float at) {
    Report(ctx, "Preparing the build", at);
    if (RunLogged(ctx, Cmd({tools.cmake, "--preset", preset}, l.source_dir, &env)) != 0) {
      if (ctx.error.empty()) ctx.error = "Preparing the build failed (CMake). See the log.";
      return false;
    }
    return true;
  };
  // Ninja lines drive the bar between `from` and `to`.
  auto build = [&](std::vector<std::string> args, const std::string& label, float from, float to) {
    Report(ctx, label, from);
    return RunLogged(ctx, Cmd(std::move(args), l.source_dir, &env), [&](std::string_view line) {
      int done = 0, total = 0;
      if (NinjaProgress(line, done, total)) {
        if (ctx.progress) ctx.progress(from + (to - from) * float(done) / float(total));
        if (ctx.counter) ctx.counter(done, total);
        if (ctx.stage) ctx.stage(label + "  " + std::to_string(done) + " / " + std::to_string(total));
      }
      if (util::ContainsI(line, "Generating recompiled code") && ctx.stage) ctx.stage("Converting the game's code");
    });
  };

  if (!configure(0.f)) return false;
  // The converted code only joins the build once CMake has seen it: convert first,
  // then configure again. Conversion re-runs only when its inputs change.
  if (build({tools.cmake, "--build", build_dir, "--target", "gundam_operation_troy_codegen", "-j", std::to_string(jobs)},
            "Building the code converter", 0.02f, 0.12f) != 0) {
    if (ctx.error.empty()) ctx.error = "Converting the game's code failed. See the log.";
    return false;
  }
  if (!util::IsFile(l.source_dir / "generated" / "default" / "sources.cmake")) {
    ctx.error = "The game's code was not converted (generated/default/sources.cmake is missing).";
    return false;
  }
  if (!configure(0.12f)) return false;
  if (build({tools.cmake, "--build", build_dir, "-j", std::to_string(jobs)}, "Compiling", 0.14f, 1.f) != 0) {
    if (ctx.error.empty()) ctx.error = "Compiling failed. See the log for the first error.";
    return false;
  }
  if (!util::IsFile(l.GameExe())) {
    ctx.error = "The build finished but " + std::string(toolchain::GameExeFileName()) + " is missing.";
    return false;
  }
  util::WriteTextFile(l.build_dir / kBuiltMarker, std::to_string(util::NowUnix()) + "\n");
  ctx.log->Add("Build finished: " + util::PathStr(l.GameExe()));
  if (ctx.progress) ctx.progress(1.f);
  return true;
}

bool Update(JobContext& ctx, toolchain::Info& tools, bool* changed) {
  const Layout& l = ctx.layout;
  *changed = false;
  if (util::Exists(l.source_dir / ".git") && !tools.git.empty()) {
    Report(ctx, "Checking for updates", -1.f);
    const std::string before = GitHead(ctx, tools);
    const proc::Env env = GitEnv();
    if (l.dev) {
      // The player's own repository copy: never throw away their changes.
      if (RunLogged(ctx, Cmd(Git(tools, {"pull", "--ff-only"}), l.source_dir, &env)) != 0) {
        if (ctx.error.empty())
          ctx.error = "Getting the update failed (git pull). If you changed files in the source, undo those changes.";
        return false;
      }
    } else {
      // The launcher's own copy (RecompData\source): nobody edits it, but the build rewrites some
      // tracked files (generated/rexglue.cmake, with different line endings), which made
      // "git pull" refuse to update. Fetch, then reset to the latest version; the extracted
      // game, the build and the converted code are untracked and stay.
      if (RunLogged(ctx, Cmd(Git(tools, {"fetch", "origin"}), l.source_dir, &env)) != 0) {
        if (ctx.error.empty()) ctx.error = "Getting the update failed (git fetch). Check your internet connection.";
        return false;
      }
      std::string changed_files, err;
      proc::Capture(Cmd(Git(tools, {"status", "--porcelain", "--untracked-files=no"}), l.source_dir, &env),
                    &changed_files, &err);
      if (!util::Trim(changed_files).empty())
        ctx.log->Add("Replacing locally changed files with the downloaded version:\n" + util::Trim(changed_files));
      if (RunLogged(ctx, Cmd(Git(tools, {"reset", "--hard", "@{u}"}), l.source_dir, &env)) != 0) {
        if (ctx.error.empty()) ctx.error = "Getting the update failed (git reset).";
        return false;
      }
    }
    const std::string after = GitHead(ctx, tools);
    *changed = before != after;
    ctx.log->Add(*changed ? "Updated " + before + " -> " + after : "Already up to date (" + after + ").");
    if (*changed && RunLogged(ctx, Cmd(Git(tools, {"submodule", "update", "--init", "--depth", "1", "--jobs", "8"}),
                                       l.source_dir, &env)) != 0) {
      if (ctx.error.empty()) ctx.error = "Updating the helper libraries failed.";
      return false;
    }
    if (!GitHelperLibs(ctx, tools)) return false;
  } else {
    if (tools.curl.empty()) {
      ctx.error = "curl was not found, so updates cannot be checked.";
      return false;
    }
    Report(ctx, "Checking for updates", -1.f);
    std::string out, err;
    proc::Capture(Cmd({tools.curl, "-sSLI", "--fail", "-o", kNullDevice, "-w", "%{url_effective}",
                       kRepoUrl + "/releases/latest"}),
                  &out, &err);
    const std::string url = util::Trim(out);
    const size_t tag_at = url.rfind("/tag/");
    if (tag_at == std::string::npos) {
      ctx.error = "No release has been published yet.";
      ctx.log->Add(ctx.error + " (" + url + ")");
      return false;
    }
    const std::string latest = url.substr(tag_at + 5);
    const std::string current = steps::SourceVersion(l);
    ctx.log->Add("Installed: " + (current.empty() ? std::string("unknown") : current) + ", latest: " + latest);
    if (latest != current) {
      const fs::path archive = l.data_dir / kSourceArchive;
      if (!Download(ctx, tools, kRepoUrl + "/releases/download/" + latest + "/" + kSourceArchive, archive,
                    "update " + latest, 0.f, 0.9f))
        return false;
      Report(ctx, "Unpacking the update", -1.f);
      if (!Unpack(ctx, tools, archive, l.source_dir)) return false;
      std::error_code ec;
      fs::remove(archive, ec);
      *changed = true;
    }
  }
  if (*changed || !BuildReady(l)) return BuildGame(ctx, tools);
  return true;
}

}  // namespace steps
