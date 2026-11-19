/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/xam/live/live_api.h
 * @brief       Client for a Xenia Web Services server (the Xbox Live replacement)
 *
 * Ported from Xenia Canary Netplay's XLiveAPI (AdrianCassar/xenia-canary,
 * branch netplay_canary_experimental, src/xenia/kernel/XLiveAPI.{h,cpp} and
 * the files in src/xenia/kernel/json/). The REST calls, their JSON fields and formats are
 * kept the same, so the recomp talks to the same servers as Xenia Netplay
 * (https://github.com/AdrianCassar/Xenia-WebServices).
 *
 * Unlike XLiveAPI, this class does not reach into the kernel: callers pass in
 * the title ID, addresses and profile data. That keeps it usable (and
 * testable) on its own; the kernel glue fills those in.
 *
 * @modified    2026, Gundam Operation Troy recomp - adapted for ReXGlue
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::kernel::xam::live {

//=============================================================================
// Settings values
//=============================================================================

// Same numbers as Xenia's network_mode setting.
enum class NetworkMode : uint32_t {
  kOffline = 0,  // no network play
  kLan = 1,      // System Link games (Operation Troy has no System Link)
  kOnline = 2,   // Xbox Live replacement, through the server in api_address
};

// Accepts "offline", "lan", "online" (any case) and Xenia's "0", "1", "2".
std::optional<NetworkMode> ParseNetworkMode(std::string_view text);
std::string_view NetworkModeName(NetworkMode mode);

// The public Xenia Netplay server, as used by Xenia Canary Netplay.
inline constexpr std::string_view kPublicServer = "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/";
// Xenia Web Services listens on this port by default (for a server on your own network).
inline constexpr uint16_t kDefaultServerPort = 36000;
// The port Xenia Netplay reports for every player (XNADDR wPortOnline).
inline constexpr uint16_t kPlayerPort = 36000;

// "192.168.1.20:36000" -> "http://192.168.1.20:36000/"; keeps https:// addresses as they are.
std::string NormalizeServerAddress(std::string_view address);

// Xenia's machine ID for a console MAC address: 0xFA00000000000000 | mac.
uint64_t MachineIdFromMac(uint64_t mac_address);

// "AABBCCDDEEFF" (Xenia's MacAddress::to_string) <-> the 48-bit number.
std::string MacToString(uint64_t mac_address);
std::optional<uint64_t> MacFromString(std::string_view text);

// "203.0.113.42" -> "203.0.113.x", for logs (see the log_mask_ips setting).
std::string MaskIp(std::string_view ip);

//=============================================================================
// Data sent to and received from the server
//=============================================================================

struct PlayerInfo {
  uint64_t xuid = 0;          // online XUID (0009xxxxxxxxxxxx)
  std::string gamertag;
  uint64_t machine_id = 0;    // see MachineIdFromMac
  std::string host_address;   // public IPv4, as whoami reported it
  uint64_t mac_address = 0;   // 48 bits
  uint16_t port = 0;          // only in FindPlayer results
  uint64_t session_id = 0;    // only in FindPlayer results
  // Profile settings to publish, per title ID ("FFFE07D1" = dashboard),
  // each already encoded as Xenia's base64 UserSetting. Optional.
  std::map<uint32_t, std::vector<std::string>> settings;
};

struct SessionInfo {
  uint64_t session_id = 0;    // XNKID as a number (top byte 0xAE = online session)
  uint64_t host_xuid = 0;
  std::string title;          // create: game name, for the server's session list
  std::string media_id;       // create: "%08X" of the XEX media ID
  std::string version;        // create: game version text
  uint32_t flags = 0;         // XSESSION_CREATE_* flags
  uint32_t public_slots = 0;
  uint32_t private_slots = 0;
  uint32_t open_public_slots = 0;     // results only
  uint32_t open_private_slots = 0;    // results only
  uint32_t filled_public_slots = 0;   // results only
  uint32_t filled_private_slots = 0;  // results only
  uint32_t user_index = 0;    // create only
  std::string host_address;   // host's public IPv4
  uint64_t mac_address = 0;   // host's MAC
  uint16_t port = 0;          // host's port (kPlayerPort)
  std::string xlast_base64;   // create: matchmaking part of the XLast, if known
  std::vector<uint64_t> player_xuids;  // details only
};

struct SessionSearchQuery {
  uint32_t search_index = 0;   // the game's matchmaking query (procedure) index
  uint32_t results_count = 0;  // most results wanted
  uint32_t num_users = 1;      // local players who would join
  uint64_t searcher_xuid = 0;  // filters out the searcher's own sessions; 0 = none
  // Search filters, each a Xenia base64 Property (contexts and properties).
  std::vector<std::string> filters;
};

struct ArbitrationInfo {
  struct Machine {
    uint64_t machine_id = 0;
    std::vector<uint64_t> xuids;
  };
  uint32_t total_players = 0;
  std::vector<Machine> machines;
};

// Port remapping a title needs (server's src/titles/<id>/ports.json). Most have none.
struct PortMappings {
  std::vector<std::pair<uint16_t, uint16_t>> connect;  // {game port, real port}
  std::vector<std::pair<uint16_t, uint16_t>> bind;
};

// What happened in the most recent call.
struct CallResult {
  std::string what;     // e.g. "POST title/424107D6/sessions"
  int status = 0;       // HTTP status, 0 = no answer
  std::string error;    // transport error, empty if the server answered
  std::string message;  // the server's "message" field, if any
  bool success() const { return error.empty() && status >= 200 && status < 300; }
};

enum class LogLevel { kDebug, kInfo, kWarning, kError };
using LogSink = std::function<void(LogLevel, const std::string&)>;

struct LiveApiConfig {
  std::string api_address = std::string(kPublicServer);
  uint32_t title_id = 0;
  uint32_t timeout_ms = 15000;
  uint32_t connect_timeout_ms = 5000;
  bool mask_ips_in_log = true;
};

//=============================================================================
// The client
//=============================================================================

class LiveApi {
 public:
  explicit LiveApi(LiveApiConfig config, LogSink log = {});

  const LiveApiConfig& config() const { return config_; }
  // Normalized server address, always ending in '/'.
  const std::string& api_address() const { return api_address_; }
  // api_address() + path, e.g. Endpoint("whoami").
  std::string Endpoint(std::string_view path) const;

  // --- Connecting ----------------------------------------------------------

  // True if the server answers at all (HEAD on its root page).
  bool Heartbeat();
  // Our public IPv4 as the server sees it (GET whoami).
  std::optional<std::string> Whoami();
  // Online: whoami, and remembers the address. LAN: heartbeat only.
  // Returns true when the server can be used.
  bool Connect(NetworkMode mode);
  bool connected() const;
  // Public IPv4 from the last successful whoami ("" if none).
  std::string online_ip() const;

  std::optional<PortMappings> GetPortMappings();

  // --- Players ---------------------------------------------------------------

  // Registers (or updates) a player: xuid + IP + MAC identify a player.
  bool RegisterPlayer(const PlayerInfo& player);
  // Looks a player up by public IPv4.
  std::optional<PlayerInfo> FindPlayer(std::string_view host_address);
  // Deletes every player registered from this PC's public IPv4.
  // Returns {gamertag, xuid} of each deleted player; nullopt if the request failed.
  std::optional<std::vector<std::pair<std::string, uint64_t>>> DeleteMyProfiles();

  // --- Sessions (matches) ----------------------------------------------------

  bool CreateSession(const SessionInfo& session);
  std::optional<SessionInfo> GetSession(uint64_t session_id);
  std::optional<SessionInfo> GetSessionDetails(uint64_t session_id);
  // Matchmaking search. nullopt = the request failed; empty = no sessions.
  std::optional<std::vector<SessionInfo>> SearchSessions(const SessionSearchQuery& query);
  // Every session of this title (used by lobby browsers).
  std::optional<std::vector<SessionInfo>> GetTitleSessions();
  bool ModifySession(uint64_t session_id, uint32_t flags, uint32_t public_slots,
                     uint32_t private_slots);
  // members: {xuid, uses a private slot}
  bool JoinSession(uint64_t session_id, const std::vector<std::pair<uint64_t, bool>>& members);
  bool PreJoinSession(uint64_t session_id, const std::vector<uint64_t>& xuids);
  bool LeaveSession(uint64_t session_id, const std::vector<uint64_t>& xuids);
  // Host migration: new_host_xuid = 0 when the new host is a remote player.
  std::optional<SessionInfo> MigrateSession(uint64_t session_id, uint64_t new_host_xuid,
                                            std::string_view host_address, uint64_t mac_address,
                                            uint16_t port);
  std::optional<ArbitrationInfo> GetArbitration(uint64_t session_id);
  // The server only deletes sessions created from the caller's own IP.
  bool DeleteSession(uint64_t session_id);
  // Deletes all sessions this machine created (by MAC; or by IP when mac_address = 0).
  bool DeleteMySessions(uint64_t mac_address);

  // --- QoS (the small blob a host publishes for players looking at its session)

  bool PostQos(uint64_t session_id, std::span<const uint8_t> data);
  // nullopt = request failed; empty vector = no QoS data published.
  std::optional<std::vector<uint8_t>> GetQos(uint64_t session_id);

  // --- Diagnostics -------------------------------------------------------------

  CallResult last_result() const;

 private:
  struct Reply {
    CallResult result;
    std::string body;
  };

  Reply Call(std::string_view method, std::string_view path, std::string body = {},
             std::string_view content_type = "application/json", bool quiet = false);
  std::string ShowIp(std::string_view ip) const;
  void Log(LogLevel level, const std::string& text) const;
  std::string TitlePath(std::string_view rest = {}) const;
  std::string SessionPath(uint64_t session_id, std::string_view rest = {}) const;

  LiveApiConfig config_;
  std::string api_address_;
  LogSink log_;

  mutable std::mutex mutex_;
  CallResult last_result_;
  std::string online_ip_;
  bool connected_ = false;
};

}  // namespace rex::kernel::xam::live
