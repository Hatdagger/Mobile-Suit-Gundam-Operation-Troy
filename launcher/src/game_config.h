// The game's own settings file (gundam_operation_troy.toml next to the game's .exe).
// ReXGlue reads flat "name = value" lines; the game's in-game settings overlay
// rewrites the same file. Only the lines the launcher changes are touched.
#pragma once

#include <string>
#include <vector>

#include "util.h"

class GameConfig {
 public:
  void Load(const fs::path& path);
  bool Save() const;
  bool Has(const std::string& key) const { return Find(key) >= 0; }
  bool GetBool(const std::string& key, bool fallback) const;
  int GetInt(const std::string& key, int fallback) const;
  std::string GetString(const std::string& key, const std::string& fallback) const;
  void SetBool(const std::string& key, bool value) { SetRaw(key, value ? "true" : "false"); }
  void SetInt(const std::string& key, int value) { SetRaw(key, std::to_string(value)); }
  void SetString(const std::string& key, const std::string& value);

 private:
  int Find(const std::string& key) const;
  std::string Raw(const std::string& key) const;
  void SetRaw(const std::string& key, const std::string& value);

  fs::path path_;
  std::vector<std::string> lines_;
};
