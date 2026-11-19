/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @file        kernel/xam/live/live_api.cpp
 * @brief       Client for a Xenia Web Services server (see live_api.h)
 *
 * Request and response formats follow Xenia Canary Netplay's XLiveAPI.cpp and
 * json/{player,session,http_response}_object_json.cc, and the server's
 * controllers (Xenia-WebServices src/infrastructure/presentation/controllers).
 *
 * @modified    2026, Gundam Operation Troy recomp - adapted for ReXGlue
 */

#include <rex/kernel/xam/live/live_api.h>

#include <rex/net/http.h>

#ifndef RAPIDJSON_HAS_STDSTRING
#define RAPIDJSON_HAS_STDSTRING 1
#endif
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace rex::kernel::xam::live {

namespace {

using rapidjson::Document;
using rapidjson::StringBuffer;
using rapidjson::Value;
using JsonWriter = rapidjson::Writer<StringBuffer>;

std::string Hex64(uint64_t value) {
  return fmt::format("{:016X}", value);
}

std::optional<uint64_t> ParseHex(std::string_view text) {
  if (text.starts_with("0x") || text.starts_with("0X")) {
    text.remove_prefix(2);
  }
  if (text.empty() || text.size() > 16) {
    return std::nullopt;
  }
  uint64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
  if (ec != std::errc() || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// --- Reading JSON without trusting it ------------------------------------------

std::string GetString(const Value& obj, const char* key) {
  if (!obj.IsObject()) {
    return {};
  }
  const auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsString()) {
    return {};
  }
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

uint64_t GetUint(const Value& obj, const char* key) {
  if (!obj.IsObject()) {
    return 0;
  }
  const auto it = obj.FindMember(key);
  if (it == obj.MemberEnd()) {
    return 0;
  }
  const Value& v = it->value;
  if (v.IsUint64()) {
    return v.GetUint64();
  }
  if (v.IsInt64()) {
    return static_cast<uint64_t>(v.GetInt64());
  }
  if (v.IsDouble()) {
    return static_cast<uint64_t>(v.GetDouble());
  }
  if (v.IsString()) {  // some fields arrive as decimal text
    uint64_t value = 0;
    const char* s = v.GetString();
    std::from_chars(s, s + v.GetStringLength(), value, 10);
    return value;
  }
  return 0;
}

uint64_t GetHex(const Value& obj, const char* key) {
  return ParseHex(GetString(obj, key)).value_or(0);
}

bool ParseJson(const std::string& text, Document& doc) {
  if (text.empty()) {
    return false;
  }
  doc.Parse(text.c_str(), text.size());
  return !doc.HasParseError();
}

std::string ToString(const Document& doc) {
  StringBuffer buffer;
  JsonWriter writer(buffer);
  doc.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

Value Str(std::string_view text, Document::AllocatorType& alloc) {
  return Value(text.data(), static_cast<rapidjson::SizeType>(text.size()), alloc);
}

// --- Objects --------------------------------------------------------------------

// PlayerObjectJSON::Deserialize
PlayerInfo ReadPlayer(const Value& obj) {
  PlayerInfo player;
  player.xuid = GetHex(obj, "xuid");
  player.gamertag = GetString(obj, "gamertag");
  player.machine_id = GetHex(obj, "machineId");
  player.host_address = GetString(obj, "hostAddress");
  player.mac_address = MacFromString(GetString(obj, "macAddress")).value_or(0);
  player.port = static_cast<uint16_t>(GetUint(obj, "port"));
  player.session_id = GetHex(obj, "sessionId");
  return player;
}

// SessionObjectJSON::Deserialize
SessionInfo ReadSession(const Value& obj) {
  SessionInfo s;
  s.session_id = GetHex(obj, "id");
  s.host_xuid = GetHex(obj, "xuid");
  s.title = GetString(obj, "title");
  s.media_id = GetString(obj, "mediaId");
  s.version = GetString(obj, "version");
  s.flags = static_cast<uint32_t>(GetUint(obj, "flags"));
  s.public_slots = static_cast<uint32_t>(GetUint(obj, "publicSlotsCount"));
  s.private_slots = static_cast<uint32_t>(GetUint(obj, "privateSlotsCount"));
  s.user_index = static_cast<uint32_t>(GetUint(obj, "userIndex"));
  s.host_address = GetString(obj, "hostAddress");
  s.mac_address = MacFromString(GetString(obj, "macAddress")).value_or(0);
  s.port = static_cast<uint16_t>(GetUint(obj, "port"));
  s.open_public_slots = static_cast<uint32_t>(GetUint(obj, "openPublicSlotsCount"));
  s.open_private_slots = static_cast<uint32_t>(GetUint(obj, "openPrivateSlotsCount"));
  s.filled_public_slots = static_cast<uint32_t>(GetUint(obj, "filledPublicSlotsCount"));
  s.filled_private_slots = static_cast<uint32_t>(GetUint(obj, "filledPrivateSlotsCount"));
  if (obj.IsObject()) {
    const auto players = obj.FindMember("players");
    if (players != obj.MemberEnd() && players->value.IsArray()) {
      for (const Value& p : players->value.GetArray()) {
        if (const uint64_t xuid = GetHex(p, "xuid")) {
          s.player_xuids.push_back(xuid);
        }
      }
    }
  }
  return s;
}

std::optional<std::vector<SessionInfo>> ReadSessionList(const std::string& body) {
  Document doc;
  if (!ParseJson(body, doc) || !doc.IsArray()) {
    return std::nullopt;
  }
  std::vector<SessionInfo> sessions;
  for (const Value& item : doc.GetArray()) {
    if (item.IsObject()) {
      sessions.push_back(ReadSession(item));
    }
  }
  return sessions;
}

Value XuidArray(const std::vector<uint64_t>& xuids, Document::AllocatorType& alloc) {
  Value array(rapidjson::kArrayType);
  for (const uint64_t xuid : xuids) {
    array.PushBack(Str(Hex64(xuid), alloc), alloc);
  }
  return array;
}

}  // namespace

//=============================================================================
// Free functions
//=============================================================================

std::optional<NetworkMode> ParseNetworkMode(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "offline" || lower == "0") {
    return NetworkMode::kOffline;
  }
  if (lower == "lan" || lower == "systemlink" || lower == "1") {
    return NetworkMode::kLan;
  }
  if (lower == "online" || lower == "live" || lower == "2") {
    return NetworkMode::kOnline;
  }
  return std::nullopt;
}

std::string_view NetworkModeName(NetworkMode mode) {
  switch (mode) {
    case NetworkMode::kOffline:
      return "offline";
    case NetworkMode::kLan:
      return "lan";
    case NetworkMode::kOnline:
      return "online";
  }
  return "offline";
}

std::string NormalizeServerAddress(std::string_view address) {
  std::string result(address);
  // Trim spaces.
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) {
    result.pop_back();
  }
  size_t start = 0;
  while (start < result.size() && std::isspace(static_cast<unsigned char>(result[start]))) {
    ++start;
  }
  result.erase(0, start);
  if (result.empty()) {
    return result;
  }
  // Xenia accepts "IP:PORT/" without a scheme (libcurl then assumes http).
  if (result.find("://") == std::string::npos) {
    result.insert(0, "http://");
  }
  if (result.back() != '/') {
    result.push_back('/');
  }
  return result;
}

uint64_t MachineIdFromMac(uint64_t mac_address) {
  return 0xFA00000000000000ull | (mac_address & 0xFFFFFFFFFFFFull);
}

std::string MacToString(uint64_t mac_address) {
  return fmt::format("{:012X}", mac_address & 0xFFFFFFFFFFFFull);
}

std::optional<uint64_t> MacFromString(std::string_view text) {
  std::string hex;
  for (const char c : text) {
    if (c != ':' && c != '-') {
      hex.push_back(c);
    }
  }
  if (hex.size() != 12) {
    return std::nullopt;
  }
  return ParseHex(hex);
}

std::string MaskIp(std::string_view ip) {
  const size_t last_dot = ip.rfind('.');
  if (last_dot == std::string_view::npos) {
    return std::string(ip);
  }
  return std::string(ip.substr(0, last_dot)) + ".x";
}

//=============================================================================
// LiveApi
//=============================================================================

LiveApi::LiveApi(LiveApiConfig config, LogSink log)
    : config_(std::move(config)),
      api_address_(NormalizeServerAddress(config_.api_address)),
      log_(std::move(log)) {}

std::string LiveApi::Endpoint(std::string_view path) const {
  return api_address_ + std::string(path);
}

std::string LiveApi::TitlePath(std::string_view rest) const {
  return fmt::format("title/{:08X}/{}", config_.title_id, rest);
}

std::string LiveApi::SessionPath(uint64_t session_id, std::string_view rest) const {
  // Session IDs go in URLs as 16 lowercase hex digits, as in Xenia.
  if (rest.empty()) {
    return fmt::format("title/{:08X}/sessions/{:016x}", config_.title_id, session_id);
  }
  return fmt::format("title/{:08X}/sessions/{:016x}/{}", config_.title_id, session_id, rest);
}

void LiveApi::Log(LogLevel level, const std::string& text) const {
  if (log_) {
    log_(level, text);
  }
}

std::string LiveApi::ShowIp(std::string_view ip) const {
  return config_.mask_ips_in_log ? MaskIp(ip) : std::string(ip);
}

LiveApi::Reply LiveApi::Call(std::string_view method, std::string_view path, std::string body,
                             std::string_view content_type, bool quiet) {
  Reply reply;
  reply.result.what = fmt::format("{} {}", method, path);

  if (api_address_.empty()) {
    reply.result.error = "No server address is set (api_address)";
  } else {
    net::HttpRequest request;
    request.method = std::string(method);
    request.url = Endpoint(path);
    request.timeout_ms = config_.timeout_ms;
    request.connect_timeout_ms = config_.connect_timeout_ms;
    if (!body.empty() || method == "POST") {
      request.headers.emplace_back("Content-Type", std::string(content_type));
    }
    request.headers.emplace_back("Accept", "application/json");
    request.body = std::move(body);

    net::HttpResponse response = net::HttpSend(request);
    reply.result.status = response.status;
    reply.result.error = std::move(response.error);
    reply.body = std::move(response.body);

    // Error answers look like {"message": "...", "error": "Not Found", "statusCode": 404}.
    if (reply.result.error.empty() && !reply.result.success()) {
      Document doc;
      if (ParseJson(reply.body, doc) && doc.IsObject()) {
        reply.result.message = GetString(doc, "message");
        if (reply.result.message.empty()) {
          reply.result.message = GetString(doc, "error");
        }
      }
    }
  }

  if (!reply.result.success() && !quiet) {
    if (!reply.result.error.empty()) {
      Log(LogLevel::kError, fmt::format("Live: {} - {}", reply.result.what, reply.result.error));
    } else {
      Log(LogLevel::kError,
          fmt::format("Live: {} - HTTP {}{}{}", reply.result.what, reply.result.status,
                      reply.result.message.empty() ? "" : ": ", reply.result.message));
    }
  } else if (reply.result.success()) {
    Log(LogLevel::kDebug,
        fmt::format("Live: {} - HTTP {}", reply.result.what, reply.result.status));
  }

  {
    std::lock_guard lock(mutex_);
    last_result_ = reply.result;
  }
  return reply;
}

CallResult LiveApi::last_result() const {
  std::lock_guard lock(mutex_);
  return last_result_;
}

// --- Connecting ------------------------------------------------------------------

bool LiveApi::Heartbeat() {
  // XLiveAPI::Heartbeat: any answer below 400 from the server's root page.
  const Reply reply = Call("HEAD", "", {}, {}, /*quiet=*/true);
  return reply.result.error.empty() && reply.result.status >= 200 && reply.result.status < 400;
}

std::optional<std::string> LiveApi::Whoami() {
  const Reply reply = Call("GET", "whoami");
  if (!reply.result.success()) {
    return std::nullopt;
  }
  Document doc;
  if (!ParseJson(reply.body, doc)) {
    Log(LogLevel::kError, "Live: whoami answer is not JSON");
    return std::nullopt;
  }
  std::string address = GetString(doc, "address");
  if (address.empty()) {
    Log(LogLevel::kError, "Live: whoami answer has no address");
    return std::nullopt;
  }
  return address;
}

bool LiveApi::Connect(NetworkMode mode) {
  bool ok = false;
  std::string ip;
  if (mode == NetworkMode::kOnline) {
    if (auto address = Whoami()) {
      ip = std::move(*address);
      ok = true;
    }
  } else if (mode == NetworkMode::kLan) {
    // XLiveAPI::RefreshNetworkMode: LAN only needs the server for QoS lookups.
    ok = Heartbeat();
  }

  {
    std::lock_guard lock(mutex_);
    connected_ = ok;
    online_ip_ = ip;
  }

  if (mode == NetworkMode::kOffline) {
    Log(LogLevel::kInfo, "Live: offline mode");
  } else if (ok) {
    Log(LogLevel::kInfo,
        mode == NetworkMode::kOnline
            ? fmt::format("Live: connected to {} as {}", api_address_, ShowIp(ip))
            : fmt::format("Live: server {} answers (LAN mode)", api_address_));
  } else {
    Log(LogLevel::kError, fmt::format("Live: cannot reach the server {}", api_address_));
  }
  return ok;
}

bool LiveApi::connected() const {
  std::lock_guard lock(mutex_);
  return connected_;
}

std::string LiveApi::online_ip() const {
  std::lock_guard lock(mutex_);
  return online_ip_;
}

std::optional<PortMappings> LiveApi::GetPortMappings() {
  const Reply reply = Call("GET", TitlePath("ports"));
  if (!reply.result.success()) {
    return std::nullopt;
  }
  PortMappings mappings;
  Document doc;
  if (!ParseJson(reply.body, doc) || !doc.IsObject()) {
    return mappings;  // the server answers {} for most titles
  }
  auto read = [&](const char* key, std::vector<std::pair<uint16_t, uint16_t>>& out) {
    const auto it = doc.FindMember(key);
    if (it == doc.MemberEnd() || !it->value.IsArray()) {
      return;
    }
    for (const Value& entry : it->value.GetArray()) {
      out.emplace_back(static_cast<uint16_t>(GetUint(entry, "port")),
                       static_cast<uint16_t>(GetUint(entry, "mappedTo")));
    }
  };
  read("connect", mappings.connect);
  read("bind", mappings.bind);
  return mappings;
}

// --- Players -----------------------------------------------------------------------

bool LiveApi::RegisterPlayer(const PlayerInfo& player) {
  // PlayerObjectJSON::Serialize
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  doc.AddMember("xuid", Str(Hex64(player.xuid), alloc), alloc);
  doc.AddMember("machineId", Str(fmt::format("{:016x}", player.machine_id), alloc), alloc);
  doc.AddMember("hostAddress", Str(player.host_address, alloc), alloc);
  doc.AddMember("gamertag", Str(player.gamertag, alloc), alloc);
  doc.AddMember("macAddress", Str(fmt::format("{:012x}", player.mac_address), alloc), alloc);
  Value settings(rapidjson::kObjectType);
  for (const auto& [title_id, encoded] : player.settings) {
    Value list(rapidjson::kArrayType);
    for (const std::string& setting : encoded) {
      list.PushBack(Str(setting, alloc), alloc);
    }
    settings.AddMember(Str(fmt::format("{:08X}", title_id), alloc), list, alloc);
  }
  doc.AddMember("settings", settings, alloc);

  const Reply reply = Call("POST", "players", ToString(doc));
  if (reply.result.success()) {
    Log(LogLevel::kInfo, fmt::format("Live: registered player {} ({:016X})", player.gamertag,
                                     player.xuid));
  }
  return reply.result.success();
}

std::optional<PlayerInfo> LiveApi::FindPlayer(std::string_view host_address) {
  Document doc(rapidjson::kObjectType);
  doc.AddMember("hostAddress", Str(host_address, doc.GetAllocator()), doc.GetAllocator());
  const Reply reply = Call("POST", "players/find", ToString(doc));
  if (!reply.result.success()) {
    return std::nullopt;
  }
  Document answer;
  if (!ParseJson(reply.body, answer) || !answer.IsObject()) {
    return std::nullopt;
  }
  return ReadPlayer(answer);
}

std::optional<std::vector<std::pair<std::string, uint64_t>>> LiveApi::DeleteMyProfiles() {
  // The server answers [["gamertag", "XUID"], ...] (DeleteMyProfilesObjectJSON).
  const Reply reply = Call("GET", "players/deletemyprofiles");
  if (!reply.result.success()) {
    return std::nullopt;
  }
  std::vector<std::pair<std::string, uint64_t>> deleted;
  Document doc;
  if (ParseJson(reply.body, doc) && doc.IsArray()) {
    for (const Value& entry : doc.GetArray()) {
      if (entry.IsArray() && entry.Size() == 2 && entry[0].IsString() && entry[1].IsString()) {
        deleted.emplace_back(entry[0].GetString(), ParseHex(entry[1].GetString()).value_or(0));
      }
    }
  }
  return deleted;
}

// --- Sessions ----------------------------------------------------------------------

bool LiveApi::CreateSession(const SessionInfo& session) {
  // XLiveAPI::XSessionCreate / SessionObjectJSON::Serialize
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  doc.AddMember("sessionId", Str(fmt::format("{:016x}", session.session_id), alloc), alloc);
  doc.AddMember("xuid", Str(Hex64(session.host_xuid), alloc), alloc);
  doc.AddMember("title", Str(session.title, alloc), alloc);
  doc.AddMember("mediaId", Str(session.media_id, alloc), alloc);
  doc.AddMember("version", Str(session.version, alloc), alloc);
  doc.AddMember("flags", session.flags, alloc);
  doc.AddMember("publicSlotsCount", session.public_slots, alloc);
  doc.AddMember("privateSlotsCount", session.private_slots, alloc);
  doc.AddMember("userIndex", session.user_index, alloc);
  doc.AddMember("hostAddress", Str(session.host_address, alloc), alloc);
  doc.AddMember("macAddress", Str(MacToString(session.mac_address), alloc), alloc);
  doc.AddMember("port", static_cast<int>(session.port), alloc);
  doc.AddMember("xlast_src", Str(session.xlast_base64, alloc), alloc);

  const Reply reply = Call("POST", TitlePath("sessions"), ToString(doc));
  if (reply.result.success()) {
    Log(LogLevel::kInfo, fmt::format("Live: created session {:016X}", session.session_id));
  }
  return reply.result.success();
}

std::optional<SessionInfo> LiveApi::GetSession(uint64_t session_id) {
  const Reply reply = Call("GET", SessionPath(session_id));
  Document doc;
  if (!reply.result.success() || !ParseJson(reply.body, doc) || !doc.IsObject()) {
    return std::nullopt;
  }
  return ReadSession(doc);
}

std::optional<SessionInfo> LiveApi::GetSessionDetails(uint64_t session_id) {
  const Reply reply = Call("GET", SessionPath(session_id, "details"));
  Document doc;
  if (!reply.result.success() || !ParseJson(reply.body, doc) || !doc.IsObject()) {
    return std::nullopt;
  }
  return ReadSession(doc);
}

std::optional<std::vector<SessionInfo>> LiveApi::SearchSessions(const SessionSearchQuery& query) {
  // XLiveAPI::SessionSearch
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  doc.AddMember("searchIndex", query.search_index, alloc);
  doc.AddMember("resultsCount", query.results_count, alloc);
  doc.AddMember("numUsers", query.num_users, alloc);
  if (query.searcher_xuid) {
    doc.AddMember("searcher_xuid", Str(Hex64(query.searcher_xuid), alloc), alloc);
  }
  Value filters(rapidjson::kArrayType);
  for (const std::string& filter : query.filters) {
    filters.PushBack(Str(filter, alloc), alloc);
  }
  doc.AddMember("filters", filters, alloc);

  const Reply reply = Call("POST", TitlePath("sessions/search"), ToString(doc));
  if (!reply.result.success()) {
    return std::nullopt;
  }
  auto sessions = ReadSessionList(reply.body);
  if (sessions) {
    Log(LogLevel::kInfo, fmt::format("Live: session search found {}", sessions->size()));
  }
  return sessions;
}

std::optional<std::vector<SessionInfo>> LiveApi::GetTitleSessions() {
  const Reply reply = Call("GET", TitlePath("sessions/search"));
  if (!reply.result.success()) {
    return std::nullopt;
  }
  return ReadSessionList(reply.body);
}

bool LiveApi::ModifySession(uint64_t session_id, uint32_t flags, uint32_t public_slots,
                            uint32_t private_slots) {
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  doc.AddMember("flags", flags, alloc);
  doc.AddMember("publicSlotsCount", public_slots, alloc);
  doc.AddMember("privateSlotsCount", private_slots, alloc);
  return Call("POST", SessionPath(session_id, "modify"), ToString(doc)).result.success();
}

bool LiveApi::JoinSession(uint64_t session_id,
                          const std::vector<std::pair<uint64_t, bool>>& members) {
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  Value xuids(rapidjson::kArrayType);
  Value private_slots(rapidjson::kArrayType);
  for (const auto& [xuid, is_private] : members) {
    xuids.PushBack(Str(Hex64(xuid), alloc), alloc);
    private_slots.PushBack(Value(is_private), alloc);
  }
  doc.AddMember("xuids", xuids, alloc);
  doc.AddMember("privateSlots", private_slots, alloc);
  return Call("POST", SessionPath(session_id, "join"), ToString(doc)).result.success();
}

bool LiveApi::PreJoinSession(uint64_t session_id, const std::vector<uint64_t>& xuids) {
  Document doc(rapidjson::kObjectType);
  doc.AddMember("xuids", XuidArray(xuids, doc.GetAllocator()), doc.GetAllocator());
  // Xenia formats the session ID in upper case here; the server accepts either.
  return Call("POST", SessionPath(session_id, "prejoin"), ToString(doc)).result.success();
}

bool LiveApi::LeaveSession(uint64_t session_id, const std::vector<uint64_t>& xuids) {
  Document doc(rapidjson::kObjectType);
  doc.AddMember("xuids", XuidArray(xuids, doc.GetAllocator()), doc.GetAllocator());
  return Call("POST", SessionPath(session_id, "leave"), ToString(doc)).result.success();
}

std::optional<SessionInfo> LiveApi::MigrateSession(uint64_t session_id, uint64_t new_host_xuid,
                                                   std::string_view host_address,
                                                   uint64_t mac_address, uint16_t port) {
  Document doc(rapidjson::kObjectType);
  auto& alloc = doc.GetAllocator();
  doc.AddMember("xuid", Str(Hex64(new_host_xuid), alloc), alloc);
  doc.AddMember("hostAddress", Str(host_address, alloc), alloc);
  doc.AddMember("macAddress", Str(MacToString(mac_address), alloc), alloc);
  doc.AddMember("port", static_cast<int>(port), alloc);
  const Reply reply = Call("POST", SessionPath(session_id, "migrate"), ToString(doc));
  Document answer;
  if (!reply.result.success() || !ParseJson(reply.body, answer) || !answer.IsObject()) {
    return std::nullopt;
  }
  return ReadSession(answer);
}

std::optional<ArbitrationInfo> LiveApi::GetArbitration(uint64_t session_id) {
  const Reply reply = Call("GET", SessionPath(session_id, "arbitration"));
  Document doc;
  if (!reply.result.success() || !ParseJson(reply.body, doc) || !doc.IsObject()) {
    return std::nullopt;
  }
  ArbitrationInfo info;
  info.total_players = static_cast<uint32_t>(GetUint(doc, "totalPlayers"));
  const auto machines = doc.FindMember("machines");
  if (machines != doc.MemberEnd() && machines->value.IsArray()) {
    for (const Value& m : machines->value.GetArray()) {
      ArbitrationInfo::Machine machine;
      machine.machine_id = GetHex(m, "id");
      const auto players = m.IsObject() ? m.FindMember("players") : m.MemberEnd();
      if (m.IsObject() && players != m.MemberEnd() && players->value.IsArray()) {
        for (const Value& p : players->value.GetArray()) {
          machine.xuids.push_back(GetHex(p, "xuid"));
        }
      }
      info.machines.push_back(std::move(machine));
    }
  }
  return info;
}

bool LiveApi::DeleteSession(uint64_t session_id) {
  return Call("DELETE", SessionPath(session_id)).result.success();
}

bool LiveApi::DeleteMySessions(uint64_t mac_address) {
  if (mac_address == 0) {
    return Call("DELETE", "DeleteSessions").result.success();
  }
  return Call("DELETE", "DeleteSessions/" + MacToString(mac_address)).result.success();
}

// --- QoS ------------------------------------------------------------------------------

bool LiveApi::PostQos(uint64_t session_id, std::span<const uint8_t> data) {
  // Xenia posts the raw bytes with libcurl's default form content type; the
  // server keeps the raw body only for content types it parses, so match it.
  std::string body(reinterpret_cast<const char*>(data.data()), data.size());
  return Call("POST", SessionPath(session_id, "qos"), std::move(body),
              "application/x-www-form-urlencoded")
      .result.success();
}

std::optional<std::vector<uint8_t>> LiveApi::GetQos(uint64_t session_id) {
  const Reply reply = Call("GET", SessionPath(session_id, "qos"));
  if (!reply.result.success()) {
    return std::nullopt;
  }
  if (reply.result.status == 204) {
    return std::vector<uint8_t>{};
  }
  return std::vector<uint8_t>(reply.body.begin(), reply.body.end());
}

}  // namespace rex::kernel::xam::live
