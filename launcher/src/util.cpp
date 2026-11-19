#include "util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

namespace util {

namespace {

std::FILE* OpenFile(const fs::path& p, const char* mode) {
#ifdef _WIN32
  std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return std::fopen(p.c_str(), mode);
#endif
}

char LowerChar(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

}  // namespace

fs::path U8Path(std::string_view utf8) {
  std::u8string s(utf8.size(), u8'\0');
  std::copy(utf8.begin(), utf8.end(), reinterpret_cast<char*>(s.data()));
  return fs::path(s);
}

std::string PathStr(const fs::path& p) {
  std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = LowerChar(c);
  return out;
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (LowerChar(a[i]) != LowerChar(b[i])) return false;
  return true;
}

bool StartsWithI(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

bool EndsWithI(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
}

bool ContainsI(std::string_view s, std::string_view part) {
  return Lower(s).find(Lower(part)) != std::string::npos;
}

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> SplitLines(std::string_view s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n' || s[i] == '\r') {
      if (i > start) out.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::string FormatBytes(uint64_t bytes) {
  char buf[64];
  if (bytes >= (1ull << 30))
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
  else if (bytes >= (1ull << 20))
    std::snprintf(buf, sizeof buf, "%.0f MB", double(bytes) / double(1ull << 20));
  else if (bytes >= 1024)
    std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
  else
    std::snprintf(buf, sizeof buf, "%llu bytes", static_cast<unsigned long long>(bytes));
  return buf;
}

std::string FormatDuration(double seconds) {
  if (seconds < 0) seconds = 0;
  long s = long(seconds + 0.5);
  char buf[64];
  if (s < 60)
    std::snprintf(buf, sizeof buf, "%ld s", s);
  else if (s < 3600)
    std::snprintf(buf, sizeof buf, "%ld min", (s + 30) / 60);
  else
    std::snprintf(buf, sizeof buf, "%ld h %02ld min", s / 3600, (s % 3600) / 60);
  return buf;
}

std::string FormatTimestamp(int64_t unix_seconds) {
  if (unix_seconds <= 0) return "unknown";
  std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof buf, "%d %b %Y %H:%M", &tm);
  return buf;
}

int64_t NowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

int64_t FileTimeUnix(const fs::path& p) {
  std::error_code ec;
  auto ft = fs::last_write_time(p, ec);
  if (ec) return 0;
  // Portable conversion from file_clock to system_clock (no clock_cast on all compilers).
  auto now_file = fs::file_time_type::clock::now();
  auto now_sys = std::chrono::system_clock::now();
  auto sys = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - now_file);
  return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool IsDir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

bool IsFile(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

uint64_t FileSize(const fs::path& p) {
  std::error_code ec;
  auto s = fs::file_size(p, ec);
  return ec ? 0 : static_cast<uint64_t>(s);
}

bool ReadTextFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool WriteTextFile(const fs::path& p, std::string_view content) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!f) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  return !ec;
}

std::string ShortenPath(const std::string& path, size_t max_chars) {
  if (path.size() <= max_chars || max_chars < 12) return path;
  size_t keep_end = max_chars * 3 / 5;
  size_t keep_start = max_chars - keep_end - 3;
  // Do not cut inside a UTF-8 sequence.
  while (keep_start > 0 && (static_cast<unsigned char>(path[keep_start]) & 0xC0) == 0x80) --keep_start;
  size_t tail = path.size() - keep_end;
  while (tail < path.size() && (static_cast<unsigned char>(path[tail]) & 0xC0) == 0x80) ++tail;
  return path.substr(0, keep_start) + "..." + path.substr(tail);
}

// --- Log ---------------------------------------------------------------------------

Log::~Log() {
  if (file_) std::fclose(file_);
}

void Log::Open(const fs::path& file, bool truncate) {
  std::lock_guard lock(mu_);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  std::error_code ec;
  if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
  file_ = OpenFile(file, truncate ? "wb" : "ab");
  path_ = file;
}

void Log::Add(std::string_view line) {
  {
    std::lock_guard lock(mu_);
    lines_.emplace_back(line);
    if (lines_.size() > kMaxLines) lines_.pop_front();
    if (file_) {
      std::fwrite(line.data(), 1, line.size(), file_);
#ifdef _WIN32
      std::fwrite("\r\n", 1, 2, file_);
#else
      std::fwrite("\n", 1, 1, file_);
#endif
      std::fflush(file_);
    }
  }
  version_.fetch_add(1);
}

void Log::Printf(const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  Add(buf);
}

size_t Log::Count() const {
  std::lock_guard lock(mu_);
  return lines_.size();
}

std::vector<std::string> Log::Range(size_t first, size_t count) const {
  std::lock_guard lock(mu_);
  std::vector<std::string> out;
  for (size_t i = first; i < lines_.size() && out.size() < count; ++i) out.push_back(lines_[i]);
  return out;
}

std::string Log::Last() const {
  std::lock_guard lock(mu_);
  return lines_.empty() ? std::string() : lines_.back();
}

void Log::Clear() {
  {
    std::lock_guard lock(mu_);
    lines_.clear();
  }
  version_.fetch_add(1);
}

fs::path Log::FilePath() const {
  std::lock_guard lock(mu_);
  return path_;
}

// --- Settings ----------------------------------------------------------------------

void Settings::Load(const fs::path& path) {
  path_ = path;
  values_.clear();
  std::string text;
  if (!ReadTextFile(path, text)) return;
  for (const std::string& raw : SplitLines(text)) {
    std::string line = Trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    values_[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
  }
}

bool Settings::Save() const {
  std::string out = "# Operation Troy launcher settings\n";
  for (const auto& [k, v] : values_) out += k + "=" + v + "\n";
  return WriteTextFile(path_, out);
}

std::string Settings::Get(const std::string& key, const std::string& fallback) const {
  auto it = values_.find(key);
  return it == values_.end() ? fallback : it->second;
}

int Settings::GetInt(const std::string& key, int fallback) const {
  auto it = values_.find(key);
  if (it == values_.end() || it->second.empty()) return fallback;
  try {
    return std::stoi(it->second);
  } catch (...) {
    return fallback;
  }
}

bool Settings::GetBool(const std::string& key, bool fallback) const {
  auto it = values_.find(key);
  if (it == values_.end()) return fallback;
  return it->second == "1" || IEquals(it->second, "true") || IEquals(it->second, "yes");
}

void Settings::Set(const std::string& key, const std::string& value) { values_[key] = value; }

}  // namespace util
