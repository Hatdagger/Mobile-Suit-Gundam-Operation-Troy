/**
 * Session properties and contexts in Xenia's xam::Property format (base64 on
 * the wire to Xenia Web Services). No kernel dependencies, so tools can use it.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::kernel::xam::live {

//=============================================================================
// Properties (Xenia's xam::Property byte layout, base64 on the wire)
//=============================================================================

// Data types (top 4 bits of a property ID).
enum class UserDataType : uint8_t {
  kContext = 0,
  kInt32 = 1,
  kInt64 = 2,
  kDouble = 3,
  kWString = 4,
  kFloat = 5,
  kBinary = 6,
  kDateTime = 7,
  kUnset = 0xFF,
};

inline UserDataType PropertyType(uint32_t id) {
  return static_cast<UserDataType>((id >> 28) & 0xF);
}

inline constexpr uint32_t kContextGameType = 0x0000800A;
inline constexpr uint32_t kContextGameMode = 0x0000800B;
inline constexpr uint32_t kPropertyGamerPuid = 0x20008107;
inline constexpr uint32_t kPropertyGamerHostname = 0x40008109;
inline constexpr uint32_t kPropertyGamerName = 0x40008002;

// A property as Xenia serializes it: id (little-endian u32), X_USER_DATA
// (type, 3 bytes padding, 4 bytes padding, 8-byte big-endian union), then the
// string/binary bytes for wstring and binary types.
struct Property {
  uint32_t id = 0;
  UserDataType type = UserDataType::kUnset;
  uint8_t value[8] = {};       // the union, big-endian, as in guest memory
  std::vector<uint8_t> extra;  // wstring (UTF-16BE incl. terminator) / binary bytes

  bool is_context() const { return type == UserDataType::kContext; }
  bool has_extra() const { return type == UserDataType::kWString || type == UserDataType::kBinary; }
  uint32_t u32() const;

  static Property Context(uint32_t id, uint32_t value);
  static Property Int64(uint32_t id, int64_t value);
  static Property WString(uint32_t id, std::string_view utf8);
  // From guest memory: value_size bytes at data (big-endian), as XUserSetPropertyEx.
  static Property FromGuest(uint32_t id, std::span<const uint8_t> data);

  std::vector<uint8_t> Serialize() const;
  std::string ToBase64() const;
  static std::optional<Property> FromBase64(std::string_view base64);

  bool operator==(const Property& o) const {
    return id == o.id && type == o.type && std::memcmp(value, o.value, 8) == 0 && extra == o.extra;
  }
};

std::string Base64Encode(std::span<const uint8_t> data);
std::optional<std::vector<uint8_t>> Base64Decode(std::string_view text);

// The contexts and properties the local player has set.
class PropertyStore {
 public:
  static PropertyStore& Get();

  void SetContext(uint32_t id, uint32_t value);
  std::optional<uint32_t> GetContext(uint32_t id) const;
  void SetProperty(const Property& property);
  std::optional<Property> GetProperty(uint32_t id) const;
  // Adds GAME_MODE / GAME_TYPE (0) and gamer PUID / host name if missing.
  void AddDefaults(uint64_t online_xuid, const std::string& gamertag);
  // Everything, contexts included, for upload.
  std::vector<Property> All() const;
  uint64_t revision() const;

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, Property> items_;
  uint64_t revision_ = 0;
};

}  // namespace rex::kernel::xam::live
