#include "gdf.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <system_error>

namespace gdf {

namespace {

constexpr uint64_t kSector = 0x800;
constexpr char kGdfMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kGdfMagicLen = sizeof(kGdfMagic) - 1;

// SVOD (GOD) layout: 0x800-byte data blocks, a 0x1000 hash block before every
// 0x198 data blocks, plus one 0x1000 top-level hash block per Data#### file.
constexpr uint64_t kSvodHashBlock = 0x1000;
constexpr uint64_t kSvodBlocksPerL0 = 0x198;
constexpr uint64_t kSvodBlocksPerFile = 0x14388;

// Where the game data volume starts inside retail disc images (XGD1/2/3, plain).
constexpr uint64_t kIsoGdfOffsets[] = {0x0, 0xFDA0000, 0x2080000, 0x18300000};

uint16_t U16LE(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32LE(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint32_t U32BE(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }

class FileReader : public Reader {
 public:
  explicit FileReader(const fs::path& p) : f_(p, std::ios::binary) {}
  bool ok() const { return static_cast<bool>(f_); }
  bool Read(uint64_t pos, void* dst, size_t size) override {
    f_.clear();
    f_.seekg(static_cast<std::streamoff>(pos));
    f_.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return static_cast<size_t>(f_.gcount()) == size;
  }

 private:
  std::ifstream f_;
};

class GodReader : public Reader {
 public:
  bool Open(const fs::path& data_dir, std::string& error) {
    std::vector<fs::path> names;
    std::error_code ec;
    for (fs::directory_iterator it(data_dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::string n = util::PathStr(it->path().filename());
      if (n.size() == 8 && util::StartsWithI(n, "Data") && it->is_regular_file(ec)) names.push_back(it->path());
    }
    if (names.empty()) {
      error = "no Data#### files in " + util::PathStr(data_dir);
      return false;
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
      files_.push_back(std::make_unique<std::ifstream>(n, std::ios::binary));
      if (!*files_.back()) {
        error = "cannot open " + util::PathStr(n);
        return false;
      }
    }
    return true;
  }

  bool Read(uint64_t pos, void* dst, size_t size) override {
    auto* out = static_cast<char*>(dst);
    while (size > 0) {
      const uint64_t block = pos / kSector;
      const uint64_t inner = pos % kSector;
      const uint64_t file_index = block / kSvodBlocksPerFile;
      const uint64_t rem = block % kSvodBlocksPerFile;
      // Blocks within one hash group are stored back to back: read the whole run.
      uint64_t group_end = (rem / kSvodBlocksPerL0 + 1) * kSvodBlocksPerL0;
      if (group_end > kSvodBlocksPerFile) group_end = kSvodBlocksPerFile;
      const uint64_t run = (group_end - rem) * kSector - inner;
      const size_t take = static_cast<size_t>(std::min<uint64_t>(size, run));
      const uint64_t offset = rem * kSector + (rem / kSvodBlocksPerL0 + 1) * kSvodHashBlock + kSvodHashBlock;
      if (file_index >= files_.size()) return false;  // a Data file is missing
      std::ifstream& f = *files_[file_index];
      f.clear();
      f.seekg(static_cast<std::streamoff>(offset + inner));
      f.read(out, static_cast<std::streamsize>(take));
      if (static_cast<size_t>(f.gcount()) != take) return false;
      out += take;
      pos += take;
      size -= take;
    }
    return true;
  }

 private:
  std::vector<std::unique_ptr<std::ifstream>> files_;
};

bool HasGdfAt(Reader& r, uint64_t base) {
  char magic[kGdfMagicLen];
  return r.Read(base + 0x10000, magic, kGdfMagicLen) && std::memcmp(magic, kGdfMagic, kGdfMagicLen) == 0;
}

bool IsGodDataDir(const fs::path& dir) {
  return util::EndsWithI(util::PathStr(dir.filename()), ".data") && util::IsFile(dir / "Data0000");
}

bool OpenGod(const fs::path& data_dir, Source& out, std::string& error) {
  auto reader = std::make_unique<GodReader>();
  if (!reader->Open(data_dir, error)) return false;
  if (!HasGdfAt(*reader, 0)) {
    error = "the GOD data has no game volume inside (damaged or not a game)";
    return false;
  }
  out.kind = Kind::God;
  out.path = data_dir;
  out.base = 0;
  out.reader = std::move(reader);
  return true;
}

bool OpenIso(const fs::path& file, Source& out, std::string& error) {
  auto reader = std::make_unique<FileReader>(file);
  if (!reader->ok()) {
    error = "cannot open " + util::PathStr(file);
    return false;
  }
  for (uint64_t base : kIsoGdfOffsets) {
    if (HasGdfAt(*reader, base)) {
      out.kind = Kind::Iso;
      out.path = file;
      out.base = base;
      out.reader = std::move(reader);
      return true;
    }
  }
  error = "not an Xbox 360 disc image (no game volume found)";
  return false;
}

bool HasDefaultXex(const fs::path& dir) {
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (util::IEquals(util::PathStr(it->path().filename()), "default.xex")) return true;
  }
  return false;
}

// A name from the image must be one plain file or folder name.
bool SafeName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|')
      return false;
  }
  return true;
}

std::string Latin1ToUtf8(const uint8_t* p, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x80) {
      out.push_back(static_cast<char>(p[i]));
    } else {
      out.push_back(static_cast<char>(0xC0 | (p[i] >> 6)));
      out.push_back(static_cast<char>(0x80 | (p[i] & 0x3F)));
    }
  }
  return out;
}

bool ListImage(Source& src, std::vector<Entry>& entries, std::string& error) {
  uint8_t vd[kSector];
  if (!src.reader->Read(src.base + 32 * kSector, vd, sizeof vd)) {
    error = "cannot read the game volume header";
    return false;
  }
  const uint32_t root_sector = U32LE(vd + 20);
  const uint32_t root_size = U32LE(vd + 24);
  bool bad_name = false;

  std::function<bool(uint32_t, uint32_t, const std::string&, int)> walk_dir =
      [&](uint32_t sector, uint32_t size, const std::string& prefix, int depth) -> bool {
    if (size == 0 || depth > 32) return true;
    if (size > (64u << 20)) {
      error = "a folder in the image is impossibly large (damaged image?)";
      return false;
    }
    std::vector<uint8_t> data(size);
    if (!src.reader->Read(src.base + uint64_t(sector) * kSector, data.data(), size)) {
      error = "cannot read a folder of the image (is a Data file missing?)";
      return false;
    }
    std::set<size_t> seen;
    std::function<bool(size_t)> visit = [&](size_t off) -> bool {
      if (seen.count(off) || off + 14 > data.size()) return true;
      seen.insert(off);
      const uint8_t* e = data.data() + off;
      const uint16_t left = U16LE(e), right = U16LE(e + 2);
      if (left == 0xFFFF && right == 0xFFFF) return true;
      const uint32_t f_sector = U32LE(e + 4), f_size = U32LE(e + 8);
      const uint8_t attrib = e[12], name_len = e[13];
      if (off + 14 + name_len > data.size()) return true;
      const std::string name = Latin1ToUtf8(e + 14, name_len);
      if (left && !visit(size_t(left) * 4)) return false;
      if (!SafeName(name)) {
        bad_name = true;
      } else {
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        const bool is_dir = (attrib & 0x10) != 0;
        entries.push_back({path, f_sector, f_size, is_dir});
        if (is_dir && !walk_dir(f_sector, f_size, path, depth + 1)) return false;
      }
      if (right && !visit(size_t(right) * 4)) return false;
      return true;
    };
    return visit(0);
  };
  if (!walk_dir(root_sector, root_size, "", 0)) return false;
  if (bad_name) error = "some file names in the image were skipped";
  return true;
}

bool ListFolder(Source& src, std::vector<Entry>& entries, std::string& error) {
  std::error_code ec;
  fs::recursive_directory_iterator it(src.path, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) {
    error = "cannot read " + util::PathStr(src.path);
    return false;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    const std::u8string rel8 = it->path().lexically_relative(src.path).generic_u8string();
    const std::string rel(reinterpret_cast<const char*>(rel8.data()), rel8.size());
    if (rel.empty() || rel.rfind(".optroy", 0) == 0) continue;
    const bool is_dir = it->is_directory(ec);
    entries.push_back({rel, 0, is_dir ? 0 : util::FileSize(it->path()), is_dir});
  }
  return true;
}

}  // namespace

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::God: return "Games on Demand folder";
    case Kind::Iso: return "disc image";
    case Kind::Folder: return "extracted folder";
    default: return "unknown";
  }
}

bool Open(const fs::path& input, Source& out, std::string& error) {
  out = Source{};
  if (util::IsFile(input)) {
    fs::path data_dir = input;
    data_dir += ".data";
    if (util::IsDir(data_dir)) return OpenGod(data_dir, out, error);  // GOD header file
    return OpenIso(input, out, error);
  }
  if (!util::IsDir(input)) {
    error = "not found: " + util::PathStr(input);
    return false;
  }
  if (HasDefaultXex(input)) {
    out.kind = Kind::Folder;
    out.path = input;
    return true;
  }
  if (IsGodDataDir(input)) return OpenGod(input, out, error);

  // Look below the chosen folder: a GOD .data folder (preferring one under the
  // game's title ID folder), an extracted folder, or a disc image.
  std::error_code ec;
  fs::path god, folder, iso;
  fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec), end;
  int visited = 0;
  for (; !ec && it != end && visited < 20000; it.increment(ec), ++visited) {
    if (it.depth() >= 5) it.disable_recursion_pending();
    const fs::path& p = it->path();
    if (it->is_directory(ec)) {
      if (IsGodDataDir(p)) {
        const bool troy = util::ContainsI(util::PathStr(p), "424107D6");
        if (god.empty() || troy) god = p;
        it.disable_recursion_pending();
      } else if (folder.empty() && HasDefaultXex(p)) {
        folder = p;
      }
    } else if (iso.empty() && util::EndsWithI(util::PathStr(p.filename()), ".iso")) {
      iso = p;
    }
  }
  if (!god.empty()) return OpenGod(god, out, error);
  if (!folder.empty()) {
    out.kind = Kind::Folder;
    out.path = folder;
    return true;
  }
  if (!iso.empty()) return OpenIso(iso, out, error);
  error = "no Games on Demand data, disc image (.iso) or default.xex was found there";
  return false;
}

bool List(Source& src, std::vector<Entry>& entries, std::string& error) {
  entries.clear();
  if (src.kind == Kind::Folder) return ListFolder(src, entries, error);
  if (!src.reader) {
    error = "nothing opened";
    return false;
  }
  return ListImage(src, entries, error);
}

bool ReadEntry(Source& src, const Entry& entry, uint64_t offset, void* dst, size_t size) {
  if (offset + size > entry.size) return false;
  if (src.kind == Kind::Folder) {
    FileReader f(src.path / util::U8Path(entry.path));
    return f.ok() && f.Read(offset, dst, size);
  }
  return src.reader && src.reader->Read(src.base + entry.offset * kSector + offset, dst, size);
}

uint32_t ReadTitleId(Source& src, const std::vector<Entry>& entries) {
  const Entry* xex = nullptr;
  for (const auto& e : entries)
    if (!e.is_dir && util::IEquals(e.path, "default.xex")) xex = &e;
  if (!xex || xex->size < 0x20) return 0;
  std::vector<uint8_t> h(static_cast<size_t>(std::min<uint64_t>(xex->size, 0x10000)));
  if (!ReadEntry(src, *xex, 0, h.data(), h.size())) return 0;
  if (std::memcmp(h.data(), "XEX2", 4) != 0) return 0;
  const uint32_t count = std::min<uint32_t>(U32BE(h.data() + 0x14), 128);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = 0x18 + size_t(i) * 8;
    if (at + 8 > h.size()) break;
    if (U32BE(h.data() + at) == 0x00040006) {  // execution info
      const uint32_t info = U32BE(h.data() + at + 4);
      if (size_t(info) + 16 <= h.size()) return U32BE(h.data() + info + 12);
    }
  }
  return 0;
}

bool Extract(Source& src, const std::vector<Entry>& entries, const fs::path& out_dir, bool everything,
             const std::function<void(const Progress&)>& progress, const std::atomic<bool>* cancel,
             std::string& error, Progress* result) {
  std::vector<const Entry*> files;
  for (const auto& e : entries) {
    if (e.is_dir || util::StartsWithI(e.path, "$SystemUpdate/")) continue;
    if (!everything && !util::EndsWithI(e.path, ".xex")) continue;
    files.push_back(&e);
  }
  bool has_default = false;
  for (const Entry* e : files)
    if (util::IEquals(e->path, "default.xex")) has_default = true;
  if (!has_default) {
    error = "default.xex was not found in the game";
    return false;
  }

  Progress p;
  p.total_files = files.size();
  for (const Entry* e : files) p.total_bytes += e->size;

  std::error_code ec;
  fs::create_directories(out_dir, ec);
  std::vector<char> buf(8u << 20);
  for (const Entry* e : files) {
    if (cancel && cancel->load()) {
      error = "cancelled";
      return false;
    }
    const std::string out_name = util::IEquals(e->path, "default.xex") ? std::string("default.xex") : e->path;
    const fs::path dest = out_dir / util::U8Path(out_name);
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "cannot write " + util::PathStr(dest);
      return false;
    }
    p.current = e->path;
    uint64_t remaining = e->size, at = 0;
    while (remaining > 0) {
      if (cancel && cancel->load()) {
        error = "cancelled";
        return false;
      }
      const size_t take = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
      if (!ReadEntry(src, *e, at, buf.data(), take)) {
        error = "cannot read " + e->path + " from your copy (is a file missing or damaged?)";
        return false;
      }
      out.write(buf.data(), static_cast<std::streamsize>(take));
      if (!out) {
        error = "cannot write " + util::PathStr(dest) + " (is the disk full?)";
        return false;
      }
      at += take;
      remaining -= take;
      p.done_bytes += take;
      if (progress) progress(p);
    }
    ++p.done_files;
    if (progress) progress(p);
  }

  // Sanity check, as in extract_game.py.
  char magic[4] = {};
  std::ifstream check(out_dir / "default.xex", std::ios::binary);
  check.read(magic, 4);
  if (std::memcmp(magic, "XEX2", 4) != 0) {
    error = "default.xex does not start with XEX2; your copy may be damaged";
    return false;
  }
  if (result) *result = p;
  return true;
}

}  // namespace gdf
