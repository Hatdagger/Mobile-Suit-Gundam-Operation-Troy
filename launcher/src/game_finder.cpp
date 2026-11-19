#include "game_finder.h"

#include <deque>
#include <set>
#include <system_error>

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace finder {

namespace {

bool SkipFolder(const std::string& name) {
  static const char* kSkip[] = {"windows", "program files", "program files (x86)", "programdata", "appdata",
                                "$recycle.bin", "system volume information", "node_modules", ".git",
                                "recovery", "perflogs", "steamapps", "msocache", "winsxs", "recompdata"};
  if (name.empty() || name[0] == '.') return true;
  for (const char* s : kSkip)
    if (util::IEquals(name, s)) return true;
  return false;
}

bool LooksLikeTroyIso(const std::string& file_name) {
  if (!util::EndsWithI(file_name, ".iso")) return false;
  return util::ContainsI(file_name, "troy") || util::ContainsI(file_name, "gundam") ||
         util::ContainsI(file_name, "424107d6");
}

bool IsUnder(const fs::path& p, const std::vector<fs::path>& roots) {
  const std::string s = util::Lower(util::PathStr(p.lexically_normal()));
  for (const auto& r : roots) {
    if (r.empty()) continue;
    const std::string rs = util::Lower(util::PathStr(r.lexically_normal()));
    if (s.size() >= rs.size() && s.compare(0, rs.size(), rs) == 0) return true;
  }
  return false;
}

}  // namespace

bool Check(const fs::path& path, Candidate& out) {
  out = Candidate{};
  out.path = path;
  gdf::Source src;
  if (!gdf::Open(path, src, out.error)) return false;
  std::vector<gdf::Entry> entries;
  std::string list_error;
  if (!gdf::List(src, entries, list_error) && entries.empty()) {
    out.error = list_error;
    return false;
  }
  out.kind = src.kind;
  for (const auto& e : entries) {
    if (e.is_dir || util::StartsWithI(e.path, "$SystemUpdate/")) continue;
    ++out.file_count;
    out.total_bytes += e.size;
  }
  out.title_id = gdf::ReadTitleId(src, entries);
  if (out.title_id == 0) {
    out.error = "default.xex could not be read";
    return false;
  }
  return true;
}

std::vector<fs::path> DefaultRoots(const fs::path& exe_dir) {
  std::vector<fs::path> roots;
  auto add = [&](const fs::path& p) {
    if (!p.empty() && util::IsDir(p)) roots.push_back(p);
  };
  add(exe_dir);
  if (exe_dir.has_parent_path() && exe_dir.parent_path() != exe_dir) add(exe_dir.parent_path());
  for (SDL_Folder f : {SDL_FOLDER_DESKTOP, SDL_FOLDER_DOWNLOADS, SDL_FOLDER_DOCUMENTS, SDL_FOLDER_HOME}) {
    if (const char* p = SDL_GetUserFolder(f)) {
      fs::path base = util::U8Path(p);
      if (f == SDL_FOLDER_HOME) {
        add(base / "Emulation");
        add(base / "Games");
      } else {
        add(base);
      }
    }
  }
#ifdef _WIN32
  DWORD drives = GetLogicalDrives();
  for (int i = 2; i < 26; ++i) {  // C: and up
    if (!(drives & (1u << i))) continue;
    wchar_t root[4] = {wchar_t(L'A' + i), L':', L'\\', 0};
    UINT type = GetDriveTypeW(root);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
    fs::path drive(root);
    for (const char* name : {"Games", "Emulation", "Emulators", "Xbox 360", "Xenia", "ROMs", "Xenia Manager"})
      add(drive / name);
  }
#endif
  return roots;
}

bool Search(const std::vector<fs::path>& roots, const std::vector<fs::path>& exclude,
            const std::atomic<bool>* cancel, Candidate& out) {
  constexpr int kMaxDepth = 7;
  constexpr int kMaxFolders = 40000;
  std::deque<std::pair<fs::path, int>> queue;
  std::set<std::string> seen;
  for (const auto& r : roots) queue.emplace_back(r, 0);
  int folders = 0;
  while (!queue.empty() && folders < kMaxFolders) {
    if (cancel && cancel->load()) return false;
    auto [dir, depth] = queue.front();
    queue.pop_front();
    const std::string key = util::Lower(util::PathStr(dir.lexically_normal()));
    if (!seen.insert(key).second || IsUnder(dir, exclude)) continue;
    ++folders;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      const std::string name = util::PathStr(p.filename());
      std::error_code ec2;
      if (it->is_directory(ec2)) {
        if (util::IEquals(name, "424107D6")) {
          Candidate c;
          if (Check(p, c) && c.IsTroy()) {
            // Remember the game's own folder (the one holding 424107D6).
            c.path = p.parent_path();
            out = c;
            return true;
          }
          continue;
        }
        if (depth + 1 <= kMaxDepth && !SkipFolder(name)) queue.emplace_back(p, depth + 1);
      } else if (LooksLikeTroyIso(name)) {
        Candidate c;
        if (Check(p, c) && c.IsTroy()) {
          out = c;
          return true;
        }
      }
    }
  }
  return false;
}

}  // namespace finder
