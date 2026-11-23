#include "english_patch.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include "optroy_xd3.h"
#include "subprocess.h"

namespace english_patch {
namespace {

constexpr const char* kAppliedMarker = ".english_patch";      // in the game folder
constexpr const char* kWantedFile = "english_patch.txt";      // in RecompData: "on" / "off"
constexpr const char* kOriginalsDir = "english_patch_originals";  // in RecompData
constexpr const char* kTempSuffix = ".patched-tmp";

void Report(JobContext& ctx, const std::string& stage, float progress) {
  if (ctx.stage) ctx.stage(stage);
  if (ctx.progress) ctx.progress(progress);
}

bool ReadFile(const fs::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  if (size < 0) return false;
  f.seekg(0);
  out.resize(size_t(size));
  return size == 0 || bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool WriteFile(const fs::path& p, const uint8_t* data, size_t size) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
  return bool(f);
}

// VCDIFF (RFC 3284) files start with 'V' 'C' 'D' with the high bit set, then version 0.
bool IsPatchFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  unsigned char magic[4] = {};
  return f.read(reinterpret_cast<char*>(magic), 4) && magic[0] == 0xD6 && magic[1] == 0xC3 && magic[2] == 0xC4 &&
         magic[3] == 0x00;
}

bool IsArchive(const fs::path& p) {
  const std::string name = util::PathStr(p.filename());
  return util::EndsWithI(name, ".zip") || util::EndsWithI(name, ".7z") || util::EndsWithI(name, ".rar");
}

bool ReadVarint(const std::vector<uint8_t>& d, size_t& pos, uint64_t& value) {
  value = 0;
  for (int i = 0; i < 10 && pos < d.size(); ++i) {
    const uint8_t b = d[pos++];
    value = (value << 7) | (b & 0x7F);
    if (!(b & 0x80)) return true;
  }
  return false;
}

// xdelta3 writes "target/encoding/source/encoding" into the VCDIFF application header. Returns the
// file names it names (without folders), source first.
std::vector<std::string> NamesFromHeader(const std::vector<uint8_t>& d) {
  std::vector<std::string> names;
  if (d.size() < 5) return names;
  size_t pos = 4;
  const uint8_t indicator = d[pos++];
  if (indicator & 0x01) ++pos;  // secondary compressor id
  if (indicator & 0x02) {       // custom code table
    uint64_t len = 0;
    if (!ReadVarint(d, pos, len) || len > d.size() - pos) return names;
    pos += size_t(len);
  }
  if (!(indicator & 0x04)) return names;  // no application header
  uint64_t len = 0;
  if (!ReadVarint(d, pos, len) || len > d.size() - pos || len > 4096) return names;
  const std::string header(reinterpret_cast<const char*>(d.data() + pos), size_t(len));
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= header.size(); ++i) {
    if (i == header.size() || header[i] == '/') {
      parts.push_back(header.substr(start, i - start));
      start = i + 1;
    }
  }
  auto base = [](std::string s) {
    const size_t slash = s.find_last_of("\\:");
    return slash == std::string::npos ? s : s.substr(slash + 1);
  };
  if (parts.size() >= 3 && !parts[2].empty()) names.push_back(base(parts[2]));  // source
  if (!parts.empty() && !parts[0].empty()) names.push_back(base(parts[0]));     // target
  return names;
}

// "MenuTop.xzp.xdelta" -> "MenuTop.xzp".
std::string StripPatchExtension(std::string name) {
  static const char* kExts[] = {".xdelta", ".xdelta3", ".vcdiff", ".xd3", ".delta", ".patch", ".diff"};
  for (bool again = true; again;) {
    again = false;
    for (const char* ext : kExts) {
      if (util::EndsWithI(name, ext) && name.size() > std::char_traits<char>::length(ext)) {
        name.resize(name.size() - std::char_traits<char>::length(ext));
        again = true;
      }
    }
  }
  return name;
}

std::string Wanted(const Layout& l) {
  std::string text;
  util::ReadTextFile(l.data_dir / kWantedFile, text);
  return util::Trim(text);
}

void SetWanted(const Layout& l, bool on) {
  std::error_code ec;
  fs::create_directories(l.data_dir, ec);
  util::WriteTextFile(l.data_dir / kWantedFile, on ? "on\n" : "off\n");
}

std::vector<fs::path> FindPatches(const fs::path& folder) {
  std::vector<fs::path> patches;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec) && IsPatchFile(it->path())) patches.push_back(it->path());
  }
  std::sort(patches.begin(), patches.end());
  return patches;
}

// Puts back the originals saved when the patch was applied. Keeps the backups unless `drop`.
bool RestoreOriginals(JobContext& ctx, bool drop) {
  const Layout& l = ctx.layout;
  const fs::path originals = l.data_dir / kOriginalsDir;
  std::error_code ec;
  size_t restored = 0;
  if (util::IsDir(originals)) {
    for (auto it = fs::recursive_directory_iterator(originals, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const fs::path rel = fs::relative(it->path(), originals, ec);
      const fs::path target = l.game_dir / rel;
      std::error_code copy_ec;
      fs::create_directories(target.parent_path(), copy_ec);
      fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, copy_ec);
      if (copy_ec) {
        ctx.error = "Could not put back " + util::PathStr(rel) + ": " + copy_ec.message();
        ctx.log->Add(ctx.error);
        return false;
      }
      ++restored;
    }
  }
  fs::remove(l.game_dir / kAppliedMarker, ec);
  if (drop) fs::remove_all(originals, ec);
  if (restored) ctx.log->Printf("Put back %zu original game files.", restored);
  return true;
}

}  // namespace

fs::path Folder(const Layout& l) { return l.exe_dir / "English Patch"; }

Status GetStatus(const Layout& l) {
  Status s;
  const fs::path folder = Folder(l);
  std::error_code ec;
  if (util::IsDir(folder)) {
    s.patch_files = FindPatches(folder).size();
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec) && IsArchive(it->path()) &&
          !util::IsDir(folder / it->path().stem()))
        ++s.archives;
    }
  }
  std::string marker;
  if (util::ReadTextFile(l.game_dir / kAppliedMarker, marker)) {
    s.applied = true;
    for (const std::string& line : util::SplitLines(marker)) {
      if (util::StartsWithI(line, "files=")) s.applied_files = size_t(std::strtoull(line.c_str() + 6, nullptr, 10));
      if (util::StartsWithI(line, "when=")) s.applied_at = std::strtoll(line.c_str() + 5, nullptr, 10);
    }
  }
  return s;
}

bool Apply(JobContext& ctx, const toolchain::Info& tools) {
  const Layout& l = ctx.layout;
  const fs::path folder = Folder(l);
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (!steps::GameFilesReady(l)) {
    ctx.error = "Set up the game first (the game files need to be extracted).";
    return false;
  }

  // 1. Unpack downloaded archives (each into a folder of its own name, once).
  for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const fs::path archive = it->path();
    if (!it->is_regular_file(ec) || !IsArchive(archive) || util::IsDir(folder / archive.stem())) continue;
    Report(ctx, "Unpacking " + util::PathStr(archive.filename()), -1.f);
    const std::string tar = tools.tar.empty() ? proc::FindOnPath("tar", nullptr) : tools.tar;
    if (tar.empty()) {
      ctx.error = "Unpack " + util::PathStr(archive.filename()) + " into the English Patch folder first.";
      return false;
    }
    const fs::path dest = folder / archive.stem();
    fs::create_directories(dest, ec);
    std::string out, err;
    proc::Command cmd;
    cmd.args = {tar, "-xf", util::PathStr(archive), "-C", util::PathStr(dest)};
    const int code = proc::Capture(cmd, &out, &err);
    if (code != 0) {
      fs::remove_all(dest, ec);
      ctx.log->Add(out + err);
      ctx.error = "Could not unpack " + util::PathStr(archive.filename()) +
                  ". Unpack it into the English Patch folder yourself, then try again.";
      return false;
    }
    ctx.log->Add("Unpacked " + util::PathStr(archive.filename()));
  }

  // 2. The patch files.
  const std::vector<fs::path> patches = FindPatches(folder);
  if (patches.empty()) {
    ctx.error = "No patch files in the English Patch folder. Download the patch from Eight Mansions and put it there.";
    return false;
  }
  ctx.log->Printf("English patch: %zu patch files in %s", patches.size(), util::PathStr(folder).c_str());

  // 3. Start from the original files if it was applied before.
  if (util::IsFile(l.game_dir / kAppliedMarker) && !RestoreOriginals(ctx, true)) return false;

  // 4. Every game file by name (the patch only touches Contents\textures\XUI, but its folder layout
  //    may differ from ours).
  std::map<std::string, std::vector<fs::path>> by_name;  // lower-case file name -> paths (relative)
  for (auto it = fs::recursive_directory_iterator(l.game_dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = util::Lower(util::PathStr(it->path().filename()));
    if (util::EndsWithI(name, kTempSuffix) || name == kAppliedMarker) continue;
    by_name[name].push_back(fs::relative(it->path(), l.game_dir, ec));
  }

  // 5. Check and decode every patch into a temporary file next to its target. Nothing is changed
  //    unless every patch fits.
  struct Done {
    fs::path rel;  // game file, relative to the game folder
    fs::path temp;
  };
  std::vector<Done> done;
  std::vector<std::string> problems;
  auto cleanup = [&] {
    for (const Done& d : done) fs::remove(d.temp, ec);
  };
  std::vector<uint8_t> patch, source, output;
  for (size_t i = 0; i < patches.size(); ++i) {
    if (ctx.Cancelled()) {
      cleanup();
      ctx.error = "Stopped.";
      return false;
    }
    const fs::path& p = patches[i];
    const std::string patch_name = util::PathStr(fs::relative(p, folder, ec));
    Report(ctx, "Checking " + util::PathStr(p.filename()), float(i) / float(patches.size()));
    if (ctx.counter) ctx.counter(int(i), int(patches.size()));
    if (!ReadFile(p, patch)) {
      problems.push_back(patch_name + ": could not be read");
      continue;
    }
    // Which game file: the patch's own name without ".xdelta", then the names inside the patch.
    std::vector<std::string> names = {StripPatchExtension(util::PathStr(p.filename()))};
    for (const std::string& n : NamesFromHeader(patch)) names.push_back(n);
    std::vector<fs::path> candidates;
    for (const std::string& n : names) {
      auto found = by_name.find(util::Lower(n));
      if (found == by_name.end()) continue;
      for (const fs::path& rel : found->second)
        if (std::find(candidates.begin(), candidates.end(), rel) == candidates.end()) candidates.push_back(rel);
    }
    // Several game files can share a name. Rank them: inside Contents\textures\XUI (the only folder the
    // patch changes) first, then by how many trailing folders match the patch's own folders. Only the
    // best ranked are tried, so a same-named file elsewhere is never patched by mistake.
    std::vector<std::string> patch_parts;
    for (const fs::path& part : fs::relative(p, folder, ec)) patch_parts.push_back(util::Lower(util::PathStr(part)));
    if (!patch_parts.empty()) patch_parts.back() = util::Lower(StripPatchExtension(patch_parts.back()));
    auto score = [&](const fs::path& rel) {
      std::vector<std::string> parts;
      for (const fs::path& part : rel) parts.push_back(util::Lower(util::PathStr(part)));
      int s = 0;
      if (parts.size() >= 4 && parts[0] == "contents" && parts[1] == "textures" && parts[2] == "xui") s += 100;
      for (size_t k = 1; k <= std::min(parts.size(), patch_parts.size()); ++k) {
        if (parts[parts.size() - k] != patch_parts[patch_parts.size() - k]) break;
        ++s;
      }
      return s;
    };
    int best = -1;
    for (const fs::path& rel : candidates) best = std::max(best, score(rel));
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const fs::path& rel) { return score(rel) != best; }),
                     candidates.end());
    if (candidates.empty()) {
      problems.push_back(patch_name + ": no game file called " + names.front());
      continue;
    }
    bool ok = false;
    std::string why;
    for (const fs::path& rel : candidates) {
      if (!ReadFile(l.game_dir / rel, source)) {
        why = "could not read " + util::PathStr(rel);
        continue;
      }
      size_t avail = std::max<size_t>(source.size() * 2, size_t(16) << 20);
      for (;;) {
        output.resize(avail);
        size_t produced = 0;
        const char* message = nullptr;
        const int r = optroy_xd3_decode(patch.data(), patch.size(), source.data(), source.size(), output.data(),
                                        avail, &produced, &message);
        if (r == OPTROY_XD3_NO_SPACE && avail < (size_t(1) << 30)) {
          avail *= 2;
          continue;
        }
        if (r == OPTROY_XD3_OK) {
          const fs::path temp = l.game_dir / (util::PathStr(rel) + kTempSuffix);
          if (!WriteFile(temp, output.data(), produced)) {
            why = "could not write " + util::PathStr(temp);
            break;
          }
          done.push_back({rel, temp});
          ok = true;
        } else {
          why = message && *message ? message : "does not fit";
        }
        break;
      }
      if (ok) break;
    }
    if (!ok) problems.push_back(patch_name + ": " + why);
  }
  if (!problems.empty()) {
    cleanup();
    for (const std::string& line : problems) ctx.log->Add("  " + line);
    ctx.error_lines = problems;
    if (ctx.error_lines.size() > 6) ctx.error_lines.resize(6);
    ctx.error = std::to_string(problems.size()) + " of " + std::to_string(patches.size()) +
                " patch files did not fit your game files, so nothing was changed. Your copy may already be "
                "English, or a different version than the patch expects.";
    return false;
  }

  // 6. Keep the originals, then swap the patched files in.
  Report(ctx, "Applying the English patch", 0.95f);
  const fs::path originals = l.data_dir / kOriginalsDir;
  for (const Done& d : done) {
    const fs::path target = l.game_dir / d.rel;
    const fs::path backup = originals / d.rel;
    fs::create_directories(backup.parent_path(), ec);
    std::error_code copy_ec;
    fs::copy_file(target, backup, fs::copy_options::overwrite_existing, copy_ec);
    if (!copy_ec) fs::rename(d.temp, target, copy_ec);
    if (copy_ec) {
      ctx.error = "Could not replace " + util::PathStr(d.rel) + ": " + copy_ec.message();
      ctx.log->Add(ctx.error);
      cleanup();
      RestoreOriginals(ctx, true);
      return false;
    }
  }
  util::WriteTextFile(l.game_dir / kAppliedMarker,
                      "patch=Eight Mansions English patch\nfiles=" + std::to_string(done.size()) +
                          "\nwhen=" + std::to_string(util::NowUnix()) + "\n");
  SetWanted(l, true);
  ctx.log->Printf("English patch applied to %zu game files (originals kept in %s).", done.size(),
                  util::PathStr(originals).c_str());
  Report(ctx, "English patch applied", 1.f);
  return true;
}

bool Remove(JobContext& ctx) {
  Report(ctx, "Putting the original game files back", -1.f);
  if (!RestoreOriginals(ctx, true)) return false;
  SetWanted(ctx.layout, false);
  ctx.log->Add("English patch removed.");
  return true;
}

bool AfterExtract(JobContext& ctx, const toolchain::Info& tools) {
  const Layout& l = ctx.layout;
  std::error_code ec;
  // The extracted files are the originals again.
  fs::remove(l.game_dir / kAppliedMarker, ec);
  fs::remove_all(l.data_dir / kOriginalsDir, ec);
  if (Wanted(l) == "off" || !util::IsDir(Folder(l))) return true;
  const Status s = GetStatus(l);
  if (!s.patch_files && !s.archives) return true;
  ctx.log->Add("Applying the English patch to the new game files.");
  if (!Apply(ctx, tools)) {
    // The game still works in Japanese; say why in the log, but don't fail setup.
    ctx.log->Add("English patch not applied: " + ctx.error);
    ctx.error.clear();
    ctx.error_lines.clear();
  }
  return true;
}

}  // namespace english_patch
