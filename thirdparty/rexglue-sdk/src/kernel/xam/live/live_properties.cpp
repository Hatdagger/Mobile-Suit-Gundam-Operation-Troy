/**
 * Session properties and contexts (Xenia's xam::Property format). See live_session.h.
 */

#include <rex/kernel/xam/live/live_properties.h>

#include <algorithm>
#include <cstring>

namespace rex::kernel::xam::live {

namespace {
constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

//=============================================================================
// Base64
//=============================================================================

std::string Base64Encode(std::span<const uint8_t> data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kBase64Chars[(v >> 18) & 63]);
    out.push_back(kBase64Chars[(v >> 12) & 63]);
    out.push_back(kBase64Chars[(v >> 6) & 63]);
    out.push_back(kBase64Chars[v & 63]);
  }
  if (i < data.size()) {
    uint32_t v = uint32_t(data[i]) << 16;
    if (i + 1 < data.size()) {
      v |= uint32_t(data[i + 1]) << 8;
    }
    out.push_back(kBase64Chars[(v >> 18) & 63]);
    out.push_back(kBase64Chars[(v >> 12) & 63]);
    out.push_back(i + 1 < data.size() ? kBase64Chars[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text) {
  std::vector<uint8_t> out;
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') {
      break;
    }
    const char* p = std::strchr(kBase64Chars, c);
    if (!p || !c) {
      return std::nullopt;
    }
    buffer = (buffer << 6) | uint32_t(p - kBase64Chars);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

//=============================================================================
// Property
//=============================================================================

uint32_t Property::u32() const {
  return (uint32_t(value[0]) << 24) | (uint32_t(value[1]) << 16) | (uint32_t(value[2]) << 8) |
         value[3];
}

Property Property::Context(uint32_t id, uint32_t v) {
  Property p;
  p.id = id;
  p.type = UserDataType::kContext;
  p.value[0] = uint8_t(v >> 24);
  p.value[1] = uint8_t(v >> 16);
  p.value[2] = uint8_t(v >> 8);
  p.value[3] = uint8_t(v);
  return p;
}

Property Property::Int64(uint32_t id, int64_t v) {
  Property p;
  p.id = id;
  p.type = UserDataType::kInt64;
  for (int i = 7; i >= 0; --i) {
    p.value[i] = uint8_t(uint64_t(v) & 0xFF);
    v = int64_t(uint64_t(v) >> 8);
  }
  return p;
}

Property Property::WString(uint32_t id, std::string_view utf8) {
  Property p;
  p.id = id;
  p.type = UserDataType::kWString;
  // Gamertags are ASCII; widen byte by byte (UTF-16 big-endian) plus terminator.
  for (unsigned char c : utf8) {
    p.extra.push_back(0);
    p.extra.push_back(c < 0x80 ? c : '?');
  }
  p.extra.push_back(0);
  p.extra.push_back(0);
  const uint32_t size = uint32_t(p.extra.size());
  p.value[0] = uint8_t(size >> 24);
  p.value[1] = uint8_t(size >> 16);
  p.value[2] = uint8_t(size >> 8);
  p.value[3] = uint8_t(size);
  return p;
}

Property Property::FromGuest(uint32_t id, std::span<const uint8_t> data) {
  Property p;
  p.id = id;
  p.type = PropertyType(id);
  if (p.has_extra()) {
    const size_t size = std::min<size_t>(data.size(), 0x3E8);
    p.extra.assign(data.begin(), data.begin() + size);
    p.value[0] = uint8_t(size >> 24);
    p.value[1] = uint8_t(size >> 16);
    p.value[2] = uint8_t(size >> 8);
    p.value[3] = uint8_t(size);
  } else {
    std::memcpy(p.value, data.data(), std::min<size_t>(8, data.size()));
  }
  return p;
}

std::vector<uint8_t> Property::Serialize() const {
  std::vector<uint8_t> out(20, 0);
  out[0] = uint8_t(id);
  out[1] = uint8_t(id >> 8);
  out[2] = uint8_t(id >> 16);
  out[3] = uint8_t(id >> 24);
  out[4] = static_cast<uint8_t>(type);
  std::memcpy(out.data() + 12, value, 8);
  if (has_extra()) {
    out.insert(out.end(), extra.begin(), extra.end());
  }
  return out;
}

std::string Property::ToBase64() const {
  return Base64Encode(Serialize());
}

std::optional<Property> Property::FromBase64(std::string_view base64) {
  auto bytes = Base64Decode(base64);
  if (!bytes || bytes->size() < 20) {
    return std::nullopt;
  }
  const auto& b = *bytes;
  Property p;
  p.id = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
  p.type = static_cast<UserDataType>(b[4]);
  std::memcpy(p.value, b.data() + 12, 8);
  if (p.has_extra()) {
    p.extra.assign(b.begin() + 20, b.end());
  }
  return p;
}

//=============================================================================
// PropertyStore
//=============================================================================

PropertyStore& PropertyStore::Get() {
  static PropertyStore store;
  return store;
}

void PropertyStore::SetContext(uint32_t id, uint32_t value) {
  SetProperty(Property::Context(id, value));
}

std::optional<uint32_t> PropertyStore::GetContext(uint32_t id) const {
  std::lock_guard lock(mutex_);
  auto it = items_.find(id);
  if (it == items_.end()) {
    return std::nullopt;
  }
  return it->second.u32();
}

void PropertyStore::SetProperty(const Property& property) {
  std::lock_guard lock(mutex_);
  auto it = items_.find(property.id);
  if (it != items_.end() && it->second == property) {
    return;
  }
  items_[property.id] = property;
  ++revision_;
}

std::optional<Property> PropertyStore::GetProperty(uint32_t id) const {
  std::lock_guard lock(mutex_);
  auto it = items_.find(id);
  if (it == items_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void PropertyStore::AddDefaults(uint64_t online_xuid, const std::string& gamertag) {
  std::lock_guard lock(mutex_);
  auto add = [&](const Property& p) {
    if (!items_.count(p.id)) {
      items_[p.id] = p;
      ++revision_;
    }
  };
  add(Property::Context(kContextGameMode, 0));
  add(Property::Context(kContextGameType, 0));
  add(Property::Int64(kPropertyGamerPuid, int64_t(online_xuid)));
  add(Property::WString(kPropertyGamerHostname, gamertag));
  add(Property::WString(kPropertyGamerName, gamertag));
}

std::vector<Property> PropertyStore::All() const {
  std::lock_guard lock(mutex_);
  std::vector<Property> out;
  for (const auto& [id, p] : items_) {
    out.push_back(p);
  }
  return out;
}

uint64_t PropertyStore::revision() const {
  std::lock_guard lock(mutex_);
  return revision_;
}

}  // namespace rex::kernel::xam::live
