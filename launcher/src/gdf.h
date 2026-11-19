// Reads the player's own copy of the game: a Games on Demand (GOD) folder, an
// Xbox 360 disc image (.iso), or an already extracted folder. C++ version of
// tools/extract_game.py.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "util.h"

namespace gdf {

constexpr uint32_t kOperationTroyTitleId = 0x424107D6;

struct Entry {
  std::string path;  // "Media/Foo.bin" ('/' separated, UTF-8)
  uint64_t offset = 0;  // GDF sector (images) or unused (folders)
  uint64_t size = 0;
  bool is_dir = false;
};

class Reader {
 public:
  virtual ~Reader() = default;
  virtual bool Read(uint64_t pos, void* dst, size_t size) = 0;
};

enum class Kind { None, God, Iso, Folder };

struct Source {
  Kind kind = Kind::None;
  fs::path path;  // GOD .data folder, .iso file, or extracted folder
  std::unique_ptr<Reader> reader;
  uint64_t base = 0;  // GDF volume start inside the image
};

const char* KindName(Kind kind);

// Accepts a GOD folder (or any folder above its .data folder), a GOD header file,
// an .iso, or an extracted folder that contains default.xex.
bool Open(const fs::path& input, Source& out, std::string& error);

// Lists every file and folder in the game.
bool List(Source& src, std::vector<Entry>& entries, std::string& error);

// Reads part of one file.
bool ReadEntry(Source& src, const Entry& entry, uint64_t offset, void* dst, size_t size);

// Title ID from default.xex's header (0 if unknown).
uint32_t ReadTitleId(Source& src, const std::vector<Entry>& entries);

struct Progress {
  uint64_t done_bytes = 0, total_bytes = 0;
  size_t done_files = 0, total_files = 0;
  std::string current;
};

// Writes the game's files into out_dir. everything=false writes only the code
// modules (*.xex), which is all codegen needs.
bool Extract(Source& src, const std::vector<Entry>& entries, const fs::path& out_dir, bool everything,
             const std::function<void(const Progress&)>& progress, const std::atomic<bool>* cancel,
             std::string& error, Progress* result);

}  // namespace gdf
