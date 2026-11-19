// The work the launcher does for the player: getting the recomp source,
// installing build tools, extracting the game files, building, updating.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "toolchain.h"
#include "util.h"

// Where everything lives.
//  - Development copy: the launcher sits inside a clone of the repository; the clone
//    is the source, out\ holds the launcher's log and settings, saves go in saves\.
//  - Download: "Operation Troy.exe" alone in a folder; it keeps the recomp source,
//    the extracted game and the build in RecompData\, and saves in Saves\.
struct Layout {
  bool dev = false;
  fs::path exe_dir, source_dir, data_dir, game_dir, build_dir, saves_dir, tools_dir, log_path, settings_path;
  fs::path GameExe() const { return build_dir / toolchain::GameExeFileName(); }
  fs::path GameConfigPath() const { return build_dir / "gundam_operation_troy.toml"; }
};

Layout DetectLayout(const fs::path& exe_dir);

struct JobContext {
  Layout layout;
  util::Log* log = nullptr;
  std::atomic<bool>* cancel = nullptr;
  std::function<void(float)> progress;            // 0..1, or < 0 while unknown
  std::function<void(const std::string&)> stage;  // what is happening, in a few words
  std::function<void(int, int)> counter;          // items done / total in the current phase (for time left)
  int build_jobs = 0;                             // parallel compile jobs (0 = automatic)
  std::string error;                              // why a step failed
  std::vector<std::string> error_lines;           // compiler errors worth showing
  bool Cancelled() const { return cancel && cancel->load(); }
};

namespace steps {

// Quick checks, safe on the UI thread.
bool SourceReady(const Layout& l);      // project files present
bool HelperLibsReady(const Layout& l);  // the SDK's helper libraries (submodules) present
std::string SourceVersion(const Layout& l);
bool GameFilesReady(const Layout& l, size_t* files = nullptr, uint64_t* bytes = nullptr);
bool BuildReady(const Layout& l, int64_t* built_at = nullptr);
int AutomaticJobs();  // parallel compile jobs that fit this PC's cores and memory

// Long-running work, on a worker thread. Each returns false with ctx.error set.
bool GetSource(JobContext& ctx, const toolchain::Info& tools);
bool InstallBuildTools(JobContext& ctx, const toolchain::Info& tools);
bool ExtractGame(JobContext& ctx, const fs::path& game_copy);
bool BuildGame(JobContext& ctx, toolchain::Info& tools);
bool Update(JobContext& ctx, toolchain::Info& tools, bool* changed);

}  // namespace steps
