#include "player_name.h"

namespace player_name {

bool Allowed(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
         c == '_' || c == '.';
}

std::string Type(std::string_view current, char c) {
  std::string out(current);
  if (!Allowed(c) || out.size() >= kMaxLength) return out;
  if (c == ' ' && (out.empty() || out.back() == ' ')) return out;
  out.push_back(c);
  return out;
}

std::string Clean(std::string_view text) {
  std::string out;
  for (char c : text) {
    if (!Allowed(c)) continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out.push_back(c);
  }
  if (out.size() > kMaxLength) out.resize(kMaxLength);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace player_name
