#include "game_config.h"

void GameConfig::Load(const fs::path& path) {
  path_ = path;
  lines_.clear();
  std::string text;
  if (!util::ReadTextFile(path, text)) return;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      std::string line = text.substr(start, i - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (i < text.size() || !line.empty()) lines_.push_back(line);
      start = i + 1;
    }
  }
}

bool GameConfig::Save() const {
  std::string out;
  if (lines_.empty()) out = "# Settings for the Operation Troy recomp (written by the launcher and the game)\n";
  for (const auto& l : lines_) out += l + "\n";
  return util::WriteTextFile(path_, out);
}

int GameConfig::Find(const std::string& key) const {
  for (size_t i = 0; i < lines_.size(); ++i) {
    std::string line = util::Trim(lines_[i]);
    if (line.empty() || line[0] == '#' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq != std::string::npos && util::Trim(line.substr(0, eq)) == key) return static_cast<int>(i);
  }
  return -1;
}

std::string GameConfig::Raw(const std::string& key) const {
  int i = Find(key);
  if (i < 0) return {};
  const std::string& line = lines_[static_cast<size_t>(i)];
  std::string value = util::Trim(line.substr(line.find('=') + 1));
  // Drop a trailing comment that is not inside quotes.
  bool quoted = false;
  for (size_t k = 0; k < value.size(); ++k) {
    if (value[k] == '"') quoted = !quoted;
    if (value[k] == '#' && !quoted) {
      value = util::Trim(value.substr(0, k));
      break;
    }
  }
  return value;
}

void GameConfig::SetRaw(const std::string& key, const std::string& value) {
  const std::string line = key + " = " + value;
  int i = Find(key);
  if (i >= 0)
    lines_[static_cast<size_t>(i)] = line;
  else
    lines_.push_back(line);
}

bool GameConfig::GetBool(const std::string& key, bool fallback) const {
  std::string v = Raw(key);
  if (v == "true" || v == "1") return true;
  if (v == "false" || v == "0") return false;
  return fallback;
}

int GameConfig::GetInt(const std::string& key, int fallback) const {
  std::string v = Raw(key);
  if (v.empty()) return fallback;
  try {
    return std::stoi(v);
  } catch (...) {
    return fallback;
  }
}

std::string GameConfig::GetString(const std::string& key, const std::string& fallback) const {
  std::string v = Raw(key);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  return v.empty() ? fallback : v;
}

void GameConfig::SetString(const std::string& key, const std::string& value) {
  std::string escaped = "\"";
  for (char c : value) {
    if (c == '"' || c == '\\') escaped.push_back('\\');
    escaped.push_back(c);
  }
  escaped.push_back('"');
  SetRaw(key, escaped);
}
