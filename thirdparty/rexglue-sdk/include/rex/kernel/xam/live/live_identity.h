/**
 * @file        kernel/xam/live/live_identity.h
 * @brief       This PC's network identity, and small address helpers
 *
 * No kernel dependencies, so tools/netcheck can test these too.
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace rex::kernel::xam::live {

// This PC as an Xbox 360 on the network.
struct Identity {
  uint64_t mac_address = 0;  // 48 bits: Microsoft prefix 7C:1E:52 + 3 random bytes
  uint64_t online_xuid = 0;  // 0x0009xxxxxxxxxxxx, as Xenia Netplay makes them
};

inline constexpr const char* kIdentityFileName = "live_identity.toml";

// A new random identity (Xenia's GenerateMacAddress and GenerateXuidOnline).
Identity GenerateIdentity();
// Reads the identity file's text; nullopt if a value is missing or malformed.
std::optional<Identity> ParseIdentity(std::string_view text);
std::string FormatIdentity(const Identity& identity);
// Loads <dir>/live_identity.toml, or makes a new identity and saves it there.
// If the file cannot be written, the new identity is still returned (and used
// until the program ends).
Identity LoadOrCreateIdentity(const std::filesystem::path& dir);

// Gamertag rules: printable ASCII, spaces trimmed, at most 15 characters;
// empty becomes "Player".
std::string SanitizeGamertag(std::string_view name);

// The host part of a server address: "https://a.b/" -> "a.b", "192.168.1.20:36000" -> "192.168.1.20".
std::string HostFromServerAddress(std::string_view address);

// IPv4 text <-> address in network byte order (as in in_addr.s_addr). 0 = invalid.
std::string Ipv4ToString(uint32_t ip);
uint32_t ParseIpv4(std::string_view text);
bool IsLoopbackIp(uint32_t ip);

// The local IPv4 address (network order) this PC uses to reach target_host
// (a host name or IPv4 text): it asks the system which route it would take,
// without sending anything. 0 if it cannot tell.
uint32_t DetectLocalIp(const std::string& target_host);

}  // namespace rex::kernel::xam::live
