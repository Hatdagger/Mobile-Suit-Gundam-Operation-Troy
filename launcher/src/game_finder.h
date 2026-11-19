// Finding the player's copy of Gundam Operation Troy.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "gdf.h"
#include "util.h"

namespace finder {

struct Candidate {
  fs::path path;  // what to remember: the folder or file the game was found at
  gdf::Kind kind = gdf::Kind::None;
  uint32_t title_id = 0;
  size_t file_count = 0;
  uint64_t total_bytes = 0;
  std::string error;
  bool IsTroy() const { return title_id == gdf::kOperationTroyTitleId; }
};

// Opens and checks a folder or file the player chose (or a remembered one).
bool Check(const fs::path& path, Candidate& out);

// The places looked at automatically: beside the launcher, the user's folders,
// and common game/emulator folders on each drive.
std::vector<fs::path> DefaultRoots(const fs::path& exe_dir);

// Looks for a copy of Operation Troy (its 424107D6 GOD folder, or an .iso named
// like the game) below the roots. Folders under `exclude` are skipped.
bool Search(const std::vector<fs::path>& roots, const std::vector<fs::path>& exclude,
            const std::atomic<bool>* cancel, Candidate& out);

}  // namespace finder
