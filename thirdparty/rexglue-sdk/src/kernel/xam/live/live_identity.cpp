/**
 * @file        kernel/xam/live/live_identity.cpp
 * @brief       This PC's network identity, and small address helpers
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */

#include <rex/kernel/xam/live/live_identity.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rex::kernel::xam::live {

namespace {

// Microsoft's "Corona" (Xbox 360) MAC prefix, as Xenia uses (kCoronaOUI).
constexpr uint64_t kCoronaOui = 0x7C1E52;

std::optional<uint64_t> ParseHexValue(std::string_view text, size_t digits) {
  if (text.size() != digits) {
    return std::nullopt;
  }
  uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

void EnsureSockets() {
#if defined(_WIN32)
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);  // reference counted; never cleaned up
  });
#endif
}

}  // namespace

Identity GenerateIdentity() {
  std::random_device rnd;
  Identity id;
  id.mac_address = (kCoronaOui << 24) | (rnd() & 0xFFFFFF);
  // GenerateXuidOnline: (0x9 << 48) + random below 2^31
  id.online_xuid = (0x9ull << 48) + (rnd() % (1u << 31));
  return id;
}

std::optional<Identity> ParseIdentity(std::string_view text) {
  std::optional<uint64_t> mac;
  std::optional<uint64_t> xuid;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = Trim(line.substr(0, eq));
    std::string_view value = Trim(line.substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
    }
    if (key == "mac_address") {
      mac = ParseHexValue(value, 12);
    } else if (key == "online_xuid") {
      xuid = ParseHexValue(value, 16);
    }
  }
  if (!mac || !xuid || *mac == 0 || *xuid == 0) {
    return std::nullopt;
  }
  return Identity{*mac, *xuid};
}

std::string FormatIdentity(const Identity& identity) {
  char mac[16];
  char xuid[24];
  std::snprintf(mac, sizeof(mac), "%012llX",
                static_cast<unsigned long long>(identity.mac_address & 0xFFFFFFFFFFFFull));
  std::snprintf(xuid, sizeof(xuid), "%016llX",
                static_cast<unsigned long long>(identity.online_xuid));
  std::string text =
      "# This PC's identity for online play (made automatically).\n"
      "# Other players and the server know you by these. Keep this file to stay the\n"
      "# same player; delete it to become a new one.\n";
  text += std::string("mac_address = \"") + mac + "\"\n";
  text += std::string("online_xuid = \"") + xuid + "\"\n";
  return text;
}

Identity LoadOrCreateIdentity(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / kIdentityFileName;
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::stringstream buffer;
      buffer << in.rdbuf();
      if (auto identity = ParseIdentity(buffer.str())) {
        return *identity;
      }
    }
  }
  const Identity identity = GenerateIdentity();
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (out) {
    out << FormatIdentity(identity);
  }
  return identity;
}

std::string SanitizeGamertag(std::string_view name) {
  std::string result;
  for (const char c : name) {
    if (c >= 0x20 && c <= 0x7E) {
      result.push_back(c);
    }
  }
  std::string_view trimmed = Trim(result);
  std::string out(trimmed.substr(0, 15));
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out.empty() ? std::string("Player") : out;
}

std::string HostFromServerAddress(std::string_view address) {
  address = Trim(address);
  const size_t scheme = address.find("://");
  if (scheme != std::string_view::npos) {
    address.remove_prefix(scheme + 3);
  }
  const size_t slash = address.find('/');
  if (slash != std::string_view::npos) {
    address = address.substr(0, slash);
  }
  const size_t at = address.rfind('@');
  if (at != std::string_view::npos) {
    address.remove_prefix(at + 1);
  }
  if (!address.empty() && address.front() == '[') {  // [IPv6]:port
    const size_t close = address.find(']');
    return std::string(address.substr(1, close == std::string_view::npos ? close : close - 1));
  }
  const size_t colon = address.find(':');
  if (colon != std::string_view::npos) {
    address = address.substr(0, colon);
  }
  return std::string(address);
}

std::string Ipv4ToString(uint32_t ip) {
  const auto* b = reinterpret_cast<const uint8_t*>(&ip);  // network order: first byte first
  return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
         std::to_string(b[3]);
}

uint32_t ParseIpv4(std::string_view text) {
  text = Trim(text);
  uint8_t bytes[4] = {};
  for (int i = 0; i < 4; ++i) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (ec != std::errc() || value > 255 || end == text.data()) {
      return 0;
    }
    bytes[i] = static_cast<uint8_t>(value);
    text.remove_prefix(static_cast<size_t>(end - text.data()));
    if (i < 3) {
      if (text.empty() || text.front() != '.') {
        return 0;
      }
      text.remove_prefix(1);
    }
  }
  if (!text.empty()) {
    return 0;
  }
  uint32_t ip = 0;
  std::memcpy(&ip, bytes, 4);
  return ip;
}

bool IsLoopbackIp(uint32_t ip) {
  return reinterpret_cast<const uint8_t*>(&ip)[0] == 127;
}

uint32_t DetectLocalIp(const std::string& target_host) {
  EnsureSockets();

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(53);  // any port; nothing is sent
  uint32_t target_ip = ParseIpv4(target_host);
  if (!target_ip && !target_host.empty()) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(target_host.c_str(), nullptr, &hints, &result) == 0 && result) {
      target_ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
    }
    if (result) {
      freeaddrinfo(result);
    }
  }
  if (!target_ip || IsLoopbackIp(target_ip)) {
    target_ip = ParseIpv4("1.1.1.1");  // the default route
  }
  target.sin_addr.s_addr = target_ip;

#if defined(_WIN32)
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    return 0;
  }
#else
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) {
    return 0;
  }
#endif
  uint32_t local = 0;
  if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
    sockaddr_in name{};
    socklen_t length = sizeof(name);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&name), &length) == 0) {
      local = name.sin_addr.s_addr;
    }
  }
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
  return local;
}

}  // namespace rex::kernel::xam::live
