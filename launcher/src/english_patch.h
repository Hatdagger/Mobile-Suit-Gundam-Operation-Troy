// The fan English translation (Eight Mansions, v1.0) for players with the Japanese copy.
//
// The launcher never downloads or hosts the patch: the player gets it from the Eight Mansions
// release page and drops it (the .zip, or the .xdelta files) into the "English Patch" folder beside
// the launcher. The launcher then applies it to the extracted game files only (the player's own ISO /
// GOD folder is never changed), keeps the original files so it can be removed again, and applies it
// again by itself after the game files are extracted again.
#pragma once

#include <string>

#include "setup_steps.h"

namespace english_patch {

inline constexpr const char* kDownloadUrl = "https://github.com/Eight-Mansions/MSGOT/releases";
inline constexpr const char* kCredits =
    "English translation by Eight Mansions: Yuvi (hacking, tools, images, translation) and "
    "SnowyAria (translation), v1.0, November 2022.";

fs::path Folder(const Layout& l);  // "English Patch" beside the launcher

struct Status {
  size_t patch_files = 0;  // xdelta / VCDIFF files found in the folder
  size_t archives = 0;     // .zip / .7z / .rar not unpacked yet
  bool applied = false;    // the extracted game files are patched
  size_t applied_files = 0;
  int64_t applied_at = 0;  // unix time
};
Status GetStatus(const Layout& l);  // quick: safe on the UI thread

// Worker thread. Unpacks archives in the folder, checks every patch against the game files and only
// then changes them (all or nothing). Applying again first puts the originals back.
bool Apply(JobContext& ctx, const toolchain::Info& tools);
// Puts the original game files back.
bool Remove(JobContext& ctx);
// After the game files were extracted again: the originals are new, so old backups are dropped, and
// the patch is applied again unless the player removed it.
bool AfterExtract(JobContext& ctx, const toolchain::Info& tools);

}  // namespace english_patch
