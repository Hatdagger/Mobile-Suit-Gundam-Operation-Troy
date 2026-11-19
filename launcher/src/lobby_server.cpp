// LAN match server: a C++ port of tools/netcheck/mock_server.py (same rules,
// which follow the real Xenia Web Services server). See lobby_server.h.
#include "lobby_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "lan_discovery.h"
#include "net_util.h"

#if defined(_WIN32)
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#ifndef OPTROY_LAUNCHER_VERSION
#define OPTROY_LAUNCHER_VERSION "dev"
#endif

namespace lobby {

namespace {

using Json = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;

// Session flags (Xenia-WebServices src/domain/value-objects/SessionFlags.ts)
constexpr int64_t kHost = 1 << 0;
constexpr int64_t kPresence = 1 << 1;
constexpr int64_t kStats = 1 << 2;
constexpr int64_t kMatchmaking = 1 << 3;
constexpr int64_t kSinglePlayerWithStats = kPresence | kStats | (1 << 8) | (1 << 9) | (1 << 10);
constexpr uint32_t kGameType = 0x0000800A;
constexpr uint32_t kGameMode = 0x0000800B;

struct HttpError {
  int status;
  std::string message;
};

std::string Upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsHex(const std::string& s, size_t length) {
  if (s.size() != length) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

// A JSON string field, or nullopt.
std::optional<std::string> Str(const Json& obj, const char* key) {
  if (!obj.IsObject()) return std::nullopt;
  auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsString()) return std::nullopt;
  return std::string(it->value.GetString(), it->value.GetStringLength());
}

std::string StrOr(const Json& obj, const char* key, std::string fallback = {}) {
  return Str(obj, key).value_or(std::move(fallback));
}

// int(value) like Python: numbers and numeric strings.
int64_t Int(const Json& obj, const char* key, int64_t fallback = 0) {
  if (!obj.IsObject()) return fallback;
  auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || it->value.IsNull()) return fallback;
  const Json& v = it->value;
  if (v.IsInt64()) return v.GetInt64();
  if (v.IsUint64()) return static_cast<int64_t>(v.GetUint64());
  if (v.IsDouble()) return static_cast<int64_t>(v.GetDouble());
  if (v.IsBool()) return v.GetBool() ? 1 : 0;
  if (v.IsString()) {
    try {
      return std::stoll(v.GetString());
    } catch (...) {
      throw HttpError{500, "invalid literal for int()"};
    }
  }
  return fallback;
}

std::string Need(const std::optional<std::string>& value, size_t hex_length, const std::string& what) {
  if (!value || !IsHex(*value, hex_length)) {
    throw HttpError{500, "Invalid " + what + " " + value.value_or("undefined")};
  }
  return *value;
}

// --- base64 + Xenia properties --------------------------------------------------

int B64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string B64Encode(const std::string& in) {
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
    out += kB64[v >> 18];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += kB64[v & 63];
  }
  if (i + 1 == in.size()) {
    uint32_t v = uint8_t(in[i]) << 16;
    out += kB64[v >> 18];
    out += kB64[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
    out += kB64[v >> 18];
    out += kB64[(v >> 12) & 63];
    out += kB64[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

std::optional<std::string> B64Decode(const std::string& in) {
  if (in.size() % 4 != 0) return std::nullopt;
  std::string out;
  for (size_t i = 0; i < in.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = in[i + k];
      if (c == '=' && i + 4 == in.size() && k >= 2) {
        v[k] = 0;
        ++pad;
      } else {
        if (pad) return std::nullopt;
        v[k] = B64Value(c);
        if (v[k] < 0) return std::nullopt;
      }
    }
    const uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out += static_cast<char>(n >> 16);
    if (pad < 2) out += static_cast<char>((n >> 8) & 0xFF);
    if (pad < 1) out += static_cast<char>(n & 0xFF);
  }
  return out;
}

struct Property {
  uint32_t id = 0;
  uint8_t type = 0;
  std::string data;
};

// Xenia Property: id (LE u32), type byte at 4, value (BE) at 12, extra at 20.
Property ParseProperty(const std::string& text) {
  auto raw = B64Decode(text);
  if (!raw || B64Encode(*raw) != text || raw->size() < 12) throw HttpError{500, "Invalid base64"};
  Property p;
  p.id = uint32_t(uint8_t((*raw)[0])) | (uint32_t(uint8_t((*raw)[1])) << 8) |
         (uint32_t(uint8_t((*raw)[2])) << 16) | (uint32_t(uint8_t((*raw)[3])) << 24);
  p.type = uint8_t((*raw)[4]);
  p.data = (p.type == 4 || p.type == 6) ? (raw->size() > 20 ? raw->substr(20) : std::string())
                                         : raw->substr(12);
  return p;
}

std::optional<uint32_t> ContextValue(const std::vector<std::string>& props, uint32_t id) {
  for (const auto& text : props) {
    Property p = ParseProperty(text);
    if (p.id == id && p.data.size() >= 4) {
      return (uint32_t(uint8_t(p.data[0])) << 24) | (uint32_t(uint8_t(p.data[1])) << 16) |
             (uint32_t(uint8_t(p.data[2])) << 8) | uint32_t(uint8_t(p.data[3]));
    }
  }
  return std::nullopt;
}

bool IsHostFlags(int64_t flags) {
  return (flags & kHost) || flags == kStats || flags == kSinglePlayerWithStats;
}

std::vector<std::string> StringList(const Json& obj, const char* key) {
  std::vector<std::string> out;
  if (!obj.IsObject()) return out;
  auto it = obj.FindMember(key);
  if (it == obj.MemberEnd() || !it->value.IsArray()) return out;
  for (const auto& v : it->value.GetArray()) {
    if (v.IsString()) out.emplace_back(v.GetString(), v.GetStringLength());
    else throw HttpError{500, std::string("Invalid ") + key};
  }
  return out;
}

// --- HTTP ---------------------------------------------------------------------

struct Request {
  std::string method;
  std::string path;   // without query, trailing '/' removed
  std::string query;  // after '?'
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
  std::string client_ip;

  std::string Header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  }
  std::string ContentType() const {
    std::string t = Header("content-type");
    t = t.substr(0, t.find(';'));
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return Lower(t);
  }
  std::optional<std::string> Query(const std::string& name) const {
    size_t pos = 0;
    while (pos <= query.size()) {
      size_t end = query.find('&', pos);
      if (end == std::string::npos) end = query.size();
      const std::string part = query.substr(pos, end - pos);
      const size_t eq = part.find('=');
      if (part.substr(0, eq) == name) return eq == std::string::npos ? std::string() : part.substr(eq + 1);
      pos = end + 1;
    }
    return std::nullopt;
  }
};

struct Response {
  int status = 200;
  std::string body;
  std::string content_type;  // empty: no Content-Type header
};

const char* Reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Error";
  }
}

const char* ErrorName(int status) {
  switch (status) {
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Error";
  }
}

std::vector<std::string> SplitPath(const std::string& path) {
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < path.size()) {
    size_t end = path.find('/', pos);
    if (end == std::string::npos) end = path.size();
    if (end > pos) parts.push_back(path.substr(pos, end - pos));
    pos = end + 1;
  }
  return parts;
}

}  // namespace

// --- the server's data -----------------------------------------------------------

struct Server::State {
  struct Player {
    std::string xuid, gamertag, host_address, machine_id, mac_address;
    std::string session_id = "0000000000000000";
  };
  struct Session {
    std::string id, xuid, title, media_id, version, host_address, mac_address, creator_ip;
    int64_t flags = 0, public_slots = 0, private_slots = 0, port = 0;
    std::vector<std::pair<std::string, bool>> players;  // xuid, private (insertion order)
    std::vector<std::string> properties;
  };

  std::mutex mutex;
  std::map<std::string, Player> players;                              // xuid (upper)
  std::map<std::pair<std::string, std::string>, Session> sessions;    // (title, sid lower)
  std::map<std::pair<std::string, std::string>, std::string> qos;
  std::vector<std::string> player_order;                              // for status
  uint64_t requests = 0;
  std::mt19937_64 rng{std::random_device{}()};

  Player* PlayerByIp(const std::string& ip) {
    for (auto& [k, p] : players)
      if (p.host_address == ip) return &p;
    return nullptr;
  }

  static std::pair<int64_t, int64_t> Available(const Session& s) {
    int64_t used_private = 0, used_public = 0;
    for (const auto& [x, priv] : s.players) (priv ? used_private : used_public)++;
    return {std::max<int64_t>(0, s.public_slots - used_public), std::max<int64_t>(0, s.private_slots - used_private)};
  }

  static void WriteSession(Writer& w, const Session& s, bool details, bool with_title) {
    auto [open_public, open_private] = Available(s);
    w.StartObject();
    w.Key("id"); w.String(s.id.c_str());
    w.Key("flags"); w.Int64(s.flags);
    w.Key("hostAddress"); w.String(s.host_address.c_str());
    w.Key("macAddress"); w.String(s.mac_address.c_str());
    w.Key("publicSlotsCount"); w.Int64(s.public_slots);
    w.Key("privateSlotsCount"); w.Int64(s.private_slots);
    w.Key("openPublicSlotsCount"); w.Int64(open_public);
    w.Key("openPrivateSlotsCount"); w.Int64(open_private);
    w.Key("filledPublicSlotsCount"); w.Int64(s.public_slots - open_public);
    w.Key("filledPrivateSlotsCount"); w.Int64(s.private_slots - open_private);
    w.Key("port"); w.Int64(s.port);
    if (with_title) {
      w.Key("title"); w.String(s.title.c_str());
      w.Key("version"); w.String(s.version.c_str());
      w.Key("mediaId"); w.String(s.media_id.c_str());
      w.Key("xuid"); w.String(s.xuid.c_str());
    }
    if (details) {
      w.Key("players");
      w.StartArray();
      for (const auto& [x, priv] : s.players) {
        w.StartObject();
        w.Key("xuid"); w.String(x.c_str());
        w.EndObject();
      }
      w.EndArray();
    }
    w.EndObject();
  }

  // One request; the caller holds mutex.
  Response Route(const Request& req);
  Response RouteSessions(const Request& req, const std::string& title, std::vector<std::string> rest);
};

namespace {

Response JsonResponse(int status, const std::function<void(Writer&)>& write) {
  rapidjson::StringBuffer buffer;
  Writer w(buffer);
  write(w);
  return {status, std::string(buffer.GetString(), buffer.GetSize()), "application/json"};
}

Response Empty(int status) {
  return {status, {}, {}};
}

rapidjson::Document ParseJsonBody(const Request& req) {
  rapidjson::Document doc;
  if (req.ContentType() != "application/json") {
    doc.SetObject();  // express.json() ignores other content types
    return doc;
  }
  const std::string& body = req.body.empty() ? std::string("{}") : req.body;
  doc.Parse(body.c_str(), body.size());
  if (doc.HasParseError()) throw HttpError{400, "Unexpected token in JSON"};
  return doc;
}

}  // namespace

Response Server::State::Route(const Request& req) {
  const auto parts = SplitPath(req.path);
  const std::string& method = req.method;

  if ((method == "GET" || method == "HEAD") && parts.empty())
    return {200, "<html><body>Operation Troy LAN server</body></html>", "text/html"};
  if (method == "GET" && parts.size() == 1 && parts[0] == "whoami")
    return JsonResponse(200, [&](Writer& w) {
      w.StartObject();
      w.Key("address"); w.String(req.client_ip.c_str());
      w.EndObject();
    });

  if (!parts.empty() && parts[0] == "DeleteSessions" && method == "DELETE" && parts.size() <= 2) {
    std::optional<std::string> mac;
    if (parts.size() == 2 && IsHex(parts[1], 12)) mac = Upper(parts[1]);
    const std::string host = req.Query("hostAddress").value_or(req.client_ip);
    for (auto it = sessions.begin(); it != sessions.end();) {
      if (it->second.host_address == host && (!mac || it->second.mac_address == *mac)) it = sessions.erase(it);
      else ++it;
    }
    return Empty(200);
  }

  if (!parts.empty() && parts[0] == "players") {
    if (method == "POST" && parts.size() == 1) {
      auto body = ParseJsonBody(req);
      const std::string xuid = Upper(Need(Str(body, "xuid"), 16, "Xuid:"));
      const std::string machine = Need(Str(body, "machineId"), 16, "Xuid:");
      const std::string mac = Need(Str(body, "macAddress"), 12, "MAC Address");
      Player p;
      p.xuid = xuid;
      p.gamertag = StrOr(body, "gamertag");
      p.host_address = StrOr(body, "hostAddress");
      p.machine_id = Upper(machine);
      p.mac_address = Upper(mac);
      if (!players.count(xuid)) player_order.push_back(xuid);
      players[xuid] = p;
      return Empty(201);
    }
    if (method == "GET" && parts.size() == 2 && parts[1] == "deletemyprofiles") {
      std::vector<Player> mine;
      for (auto it = players.begin(); it != players.end();) {
        if (it->second.host_address == req.client_ip) {
          mine.push_back(it->second);
          it = players.erase(it);
        } else {
          ++it;
        }
      }
      return JsonResponse(200, [&](Writer& w) {
        w.StartArray();
        for (const auto& p : mine) {
          w.StartArray();
          w.String(p.gamertag.c_str());
          w.String(p.xuid.c_str());
          w.EndArray();
        }
        w.EndArray();
      });
    }
    if (method == "POST" && parts.size() == 2 && parts[1] == "find") {
      auto body = ParseJsonBody(req);
      Player* p = PlayerByIp(StrOr(body, "hostAddress", "\x01"));
      if (!p) throw HttpError{404, "Player not found."};
      return JsonResponse(201, [&](Writer& w) {
        w.StartObject();
        w.Key("xuid"); w.String(p->xuid.c_str());
        w.Key("gamertag"); w.String(p->gamertag.c_str());
        w.Key("hostAddress"); w.String(p->host_address.c_str());
        w.Key("machineId"); w.String(p->machine_id.c_str());
        w.Key("port"); w.Int(36000);
        w.Key("macAddress"); w.String(p->mac_address.c_str());
        w.Key("sessionId"); w.String(p->session_id.c_str());
        w.EndObject();
      });
    }
  }

  if (!parts.empty() && parts[0] == "title" && parts.size() >= 3) {
    const std::string title = Upper(parts[1]);
    if (parts[2] == "ports" && parts.size() == 3 && method == "GET")
      return {200, "{}", "application/json"};
    if (parts[2] == "sessions")
      return RouteSessions(req, title, std::vector<std::string>(parts.begin() + 3, parts.end()));
  }

  throw HttpError{404, "Cannot " + method + " " + req.path};
}

Response Server::State::RouteSessions(const Request& req, const std::string& title,
                                      std::vector<std::string> rest) {
  const std::string& method = req.method;

  if (rest.empty() && method == "POST") {
    auto body = ParseJsonBody(req);
    const std::string sid = Lower(Need(Str(body, "sessionId"), 16, "SessionId"));
    const int64_t flags = Int(body, "flags");
    if (IsHostFlags(flags)) {
      auto xuid = Str(body, "xuid");
      Player* player = nullptr;
      if (xuid && !xuid->empty()) {
        auto it = players.find(Upper(*xuid));
        player = it == players.end() ? nullptr : &it->second;
      } else {
        player = PlayerByIp(StrOr(body, "hostAddress", "\x01"));
      }
      if (!player)
        throw HttpError{403, "Player not found: " + (xuid && !xuid->empty() ? *xuid : StrOr(body, "hostAddress"))};
      Session s;
      s.id = sid;
      s.xuid = Upper(xuid && !xuid->empty() ? *xuid : player->xuid);
      s.title = StrOr(body, "title");
      s.media_id = StrOr(body, "mediaId");
      s.version = StrOr(body, "version");
      s.flags = flags;
      s.host_address = StrOr(body, "hostAddress");
      s.mac_address = Upper(Need(Str(body, "macAddress"), 12, "MAC Address"));
      s.public_slots = Int(body, "publicSlotsCount");
      s.private_slots = Int(body, "privateSlotsCount");
      s.port = Int(body, "port");
      s.creator_ip = req.client_ip;
      sessions[{title, sid}] = std::move(s);
    }
    return Empty(201);
  }

  if (rest.size() == 1 && rest[0] == "search") {
    if (method == "GET") {
      return JsonResponse(200, [&](Writer& w) {
        w.StartArray();
        for (const auto& [key, s] : sessions)
          if (key.first == title) WriteSession(w, s, false, true);
        w.EndArray();
      });
    }
    if (method == "POST") {
      auto body = ParseJsonBody(req);
      const std::string searcher = Upper(StrOr(body, "searcher_xuid"));
      const int64_t num_users = Int(body, "numUsers");
      const bool has_filters = body.IsObject() && body.HasMember("filters") && !body["filters"].IsNull();
      const auto filters = has_filters ? StringList(body, "filters") : std::vector<std::string>();
      std::vector<const Session*> results;
      for (const auto& [key, s] : sessions) {
        auto [open_public, open_private] = Available(s);
        if (key.first != title || s.xuid == searcher || s.public_slots == 0) continue;
        if (!(s.flags & kMatchmaking) || (open_public == 0 && open_private == 0)) continue;
        if (num_users && open_public < num_users && open_private < num_users) continue;
        if (s.properties.empty()) continue;
        bool has_context = false;
        for (const auto& p : s.properties)
          if (ParseProperty(p).type == 0) has_context = true;
        if (!has_context) continue;  // no properties / contexts yet: not advertised
        if (has_filters) {
          auto want_mode = ContextValue(filters, kGameMode);
          auto want_type = ContextValue(filters, kGameType);
          if (!want_mode || !want_type) continue;
          if (ContextValue(s.properties, kGameMode) != want_mode ||
              ContextValue(s.properties, kGameType) != want_type)
            continue;
        }
        results.push_back(&s);
      }
      const int64_t count = Int(body, "resultsCount");
      if (count > 0 && static_cast<int64_t>(results.size()) > count) results.resize(static_cast<size_t>(count));
      return JsonResponse(201, [&](Writer& w) {
        w.StartArray();
        for (const auto* s : results) WriteSession(w, *s, false, false);
        w.EndArray();
      });
    }
  }

  const std::string sid = rest.empty() ? std::string() : Lower(rest[0]);
  if (!IsHex(sid, 16)) throw HttpError{500, "Invalid SessionId " + sid};
  const auto key = std::make_pair(title, sid);
  auto it = sessions.find(key);
  Session* session = it == sessions.end() ? nullptr : &it->second;
  const std::string action = rest.size() > 1 ? rest[1] : std::string();

  if (action == "qos") {
    if (method == "POST") {
      const std::string type = req.ContentType();
      if (type == "application/json") throw HttpError{400, "Unexpected token in JSON"};
      if (type != "application/x-www-form-urlencoded") throw HttpError{500, "rawBody is undefined"};
      qos[key] = req.body;
      return Empty(201);
    }
    if (method == "GET") {
      auto q = qos.find(key);
      if (q == qos.end()) return Empty(204);
      return {200, q->second, "application/octet-stream"};
    }
  }

  if (method == "DELETE" && action.empty()) {
    if (session && session->host_address == req.client_ip) {
      sessions.erase(key);
      qos.erase(key);
    }
    return Empty(200);
  }

  if (!session) throw HttpError{404, "Session " + sid + " was not found."};

  if (action == "properties") {
    if (method == "POST" && rest.size() == 2) {
      auto body = ParseJsonBody(req);
      auto props = StringList(body, "properties");
      for (const auto& p : props) ParseProperty(p);  // 500 on bad base64, like the server
      session->properties = std::move(props);
      return Empty(201);
    }
    if (method == "GET") {
      return JsonResponse(200, [&](Writer& w) {
        w.StartObject();
        w.Key("properties");
        w.StartArray();
        for (const auto& p : session->properties)
          if (ParseProperty(p).type != 0) w.String(p.c_str());
        w.EndArray();
        w.EndObject();
      });
    }
  }

  if (method == "GET" && action.empty())
    return JsonResponse(200, [&](Writer& w) { WriteSession(w, *session, false, false); });
  if (method == "GET" && action == "details")
    return JsonResponse(200, [&](Writer& w) { WriteSession(w, *session, true, false); });
  if (method == "GET" && action == "arbitration") {
    std::vector<std::pair<std::string, std::vector<std::string>>> machines;
    size_t total = 0;
    for (const auto& [xuid, priv] : session->players) {
      auto p = players.find(xuid);
      if (p == players.end()) continue;
      auto m = std::find_if(machines.begin(), machines.end(),
                            [&](const auto& e) { return e.first == p->second.machine_id; });
      if (m == machines.end()) {
        machines.push_back({p->second.machine_id, {}});
        m = machines.end() - 1;
      }
      m->second.push_back(xuid);
      ++total;
    }
    return JsonResponse(200, [&](Writer& w) {
      w.StartObject();
      w.Key("totalPlayers"); w.Uint64(total);
      w.Key("machines");
      w.StartArray();
      for (const auto& [id, xuids] : machines) {
        w.StartObject();
        w.Key("id"); w.String(id.c_str());
        w.Key("players");
        w.StartArray();
        for (const auto& x : xuids) {
          w.StartObject();
          w.Key("xuid"); w.String(x.c_str());
          w.EndObject();
        }
        w.EndArray();
        w.EndObject();
      }
      w.EndArray();
      w.EndObject();
    });
  }
  if (method == "POST" && action == "modify") {
    auto body = ParseJsonBody(req);
    session->flags = Int(body, "flags", session->flags);
    session->public_slots = Int(body, "publicSlotsCount");
    session->private_slots = Int(body, "privateSlotsCount");
    return Empty(201);
  }
  if (method == "POST" && (action == "join" || action == "prejoin")) {
    auto body = ParseJsonBody(req);
    std::vector<std::string> xuids;
    for (const auto& x : StringList(body, "xuids")) xuids.push_back(Upper(Need(x, 16, "Xuid:")));
    if (action == "join") {
      std::vector<bool> priv(xuids.size(), false);
      if (body.IsObject() && body.HasMember("privateSlots") && body["privateSlots"].IsArray()) {
        const auto& a = body["privateSlots"].GetArray();
        for (rapidjson::SizeType i = 0; i < a.Size() && i < priv.size(); ++i) priv[i] = a[i].IsBool() && a[i].GetBool();
      }
      for (size_t i = 0; i < xuids.size(); ++i) {
        auto e = std::find_if(session->players.begin(), session->players.end(),
                              [&](const auto& p) { return p.first == xuids[i]; });
        if (e != session->players.end()) e->second = priv[i];
        else session->players.push_back({xuids[i], priv[i]});
      }
    }
    for (const auto& x : xuids) {
      auto p = players.find(x);
      if (p != players.end()) p->second.session_id = sid;
    }
    return Empty(201);
  }
  if (method == "POST" && action == "leave") {
    auto body = ParseJsonBody(req);
    for (const auto& x : StringList(body, "xuids")) {
      const std::string up = Upper(Need(x, 16, "Xuid:"));
      session->players.erase(std::remove_if(session->players.begin(), session->players.end(),
                                            [&](const auto& p) { return p.first == up; }),
                             session->players.end());
    }
    return Empty(201);
  }
  if (method == "POST" && action == "migrate") {
    auto body = ParseJsonBody(req);
    Session moved = *session;
    moved.players.erase(std::remove_if(moved.players.begin(), moved.players.end(),
                                       [&](const auto& p) { return p.first == session->xuid; }),
                        moved.players.end());
    char hex[15];
    std::snprintf(hex, sizeof(hex), "%014llx", static_cast<unsigned long long>(rng() & 0xFFFFFFFFFFFFFFull));
    moved.id = sid.substr(0, 2) + hex;
    moved.host_address = StrOr(body, "hostAddress");
    moved.mac_address = Upper(Need(Str(body, "macAddress"), 12, "MAC Address"));
    moved.port = Int(body, "port");
    if (auto x = Str(body, "xuid"); x && !x->empty()) moved.xuid = Upper(Need(x, 16, "Xuid:"));
    sessions.erase(key);
    const auto new_key = std::make_pair(title, moved.id);
    auto& stored = sessions[new_key] = std::move(moved);
    return JsonResponse(201, [&](Writer& w) { WriteSession(w, stored, false, false); });
  }

  throw HttpError{404, "Cannot " + method + " " + req.path};
}

// --- networking -------------------------------------------------------------------

Server::Server() : state_(std::make_unique<State>()) {}

Server::~Server() {
  Stop();
}

bool Server::running() const {
  return running_.load();
}

Status Server::status() const {
  Status s;
  s.running = running_.load();
  s.port = port_;
  std::lock_guard lock(state_->mutex);
  for (const auto& xuid : state_->player_order) {
    auto it = state_->players.find(xuid);
    if (it != state_->players.end()) s.players.push_back(it->second.gamertag);
  }
  s.matches = static_cast<int>(state_->sessions.size());
  s.requests = state_->requests;
  return s;
}

bool Server::Start(uint16_t port, const std::string& host_name, std::string* error, uint16_t discovery_port) {
  if (running_.load()) return true;
  netu::Init();
  host_name_ = host_name;
  port_ = port;
  discovery_port_ = discovery_port;
  stopping_ = false;

  auto s = static_cast<netu::Socket>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (s == netu::kInvalidSocket) {
    if (error) *error = "Can't create a network socket (" + netu::LastErrorText() + ").";
    return false;
  }
#if !defined(_WIN32)
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 32) != 0) {
    if (error)
      *error = "Port " + std::to_string(port) +
               " is already in use. Is another LAN server (or Xenia Web Services) running on this PC?";
    netu::CloseSocket(s);
    return false;
  }
  listen_socket_ = static_cast<uintptr_t>(s);

  if (discovery_port_) {
    auto d = static_cast<netu::Socket>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (d != netu::kInvalidSocket) {
#if !defined(_WIN32)
      setsockopt(d, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
      sockaddr_in daddr{};
      daddr.sin_family = AF_INET;
      daddr.sin_port = htons(discovery_port_);
      daddr.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(d, reinterpret_cast<sockaddr*>(&daddr), sizeof(daddr)) == 0) {
        netu::SetRecvTimeout(d, 200);
        discovery_socket_ = static_cast<uintptr_t>(d);
      } else {
        netu::CloseSocket(d);  // still works; JOIN then needs the address typed in
      }
    }
  }

  running_ = true;
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  if (discovery_socket_ != ~uintptr_t(0)) discovery_thread_ = std::thread([this] { DiscoveryLoop(); });
  return true;
}

void Server::Stop() {
  if (!running_.load()) return;
  stopping_ = true;
  if (accept_thread_.joinable()) accept_thread_.join();
  if (discovery_thread_.joinable()) discovery_thread_.join();
  netu::CloseSocket(static_cast<netu::Socket>(listen_socket_));
  netu::CloseSocket(static_cast<netu::Socket>(discovery_socket_));
  listen_socket_ = ~uintptr_t(0);
  discovery_socket_ = ~uintptr_t(0);
  // Connections time out after a few seconds at most.
  for (int i = 0; i < 100 && open_connections_.load() > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  running_ = false;
}

void Server::AcceptLoop() {
  const auto ls = static_cast<netu::Socket>(listen_socket_);
  while (!stopping_.load()) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(ls, &set);
    timeval tv{0, 200 * 1000};
    if (select(static_cast<int>(ls) + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    auto c = static_cast<netu::Socket>(accept(ls, reinterpret_cast<sockaddr*>(&peer), &len));
    if (c == netu::kInvalidSocket) continue;
    ++open_connections_;
    std::thread([this, c, ip = peer.sin_addr.s_addr] {
      HandleConnection(static_cast<uintptr_t>(c), ip);
      --open_connections_;
    }).detach();
  }
}

void Server::DiscoveryLoop() {
  const auto ds = static_cast<netu::Socket>(discovery_socket_);
  char buffer[512];
  while (!stopping_.load()) {
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const int got = static_cast<int>(recvfrom(ds, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len));
    if (got <= 0) continue;
    if (std::string(buffer, static_cast<size_t>(got)) != lan::kQuery) continue;
    lan::HostInfo info;
    info.name = host_name_;
    info.port = port_;
    info.version = OPTROY_LAUNCHER_VERSION;
    {
      std::lock_guard lock(state_->mutex);
      info.players = static_cast<int>(state_->players.size());
      info.matches = static_cast<int>(state_->sessions.size());
    }
    const std::string reply = lan::FormatReply(info);
    sendto(ds, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<sockaddr*>(&from), len);
  }
}

void Server::HandleConnection(uintptr_t socket_value, uint32_t peer_ip) {
  const auto c = static_cast<netu::Socket>(socket_value);
  netu::SetRecvTimeout(c, 5000);
  int one = 1;
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

  // Read the head.
  std::string data;
  char buffer[8192];
  size_t head_end = std::string::npos;
  while ((head_end = data.find("\r\n\r\n")) == std::string::npos) {
    const int got = static_cast<int>(recv(c, buffer, sizeof(buffer), 0));
    if (got <= 0 || data.size() > 64 * 1024) {
      netu::CloseSocket(c);
      return;
    }
    data.append(buffer, static_cast<size_t>(got));
  }

  Request req;
  req.client_ip = netu::IpToString(peer_ip);
  {
    const std::string head = data.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    const std::string first = head.substr(0, line_end);
    const size_t sp1 = first.find(' ');
    const size_t sp2 = first.find(' ', sp1 + 1);
    req.method = first.substr(0, sp1);
    std::string target = sp1 == std::string::npos ? "/" : first.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    if (q != std::string::npos) {
      req.query = target.substr(q + 1);
      target = target.substr(0, q);
    }
    while (target.size() > 1 && target.back() == '/') target.pop_back();
    req.path = target.empty() ? "/" : target;
    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
      size_t end = head.find("\r\n", pos);
      if (end == std::string::npos) end = head.size();
      const std::string line = head.substr(pos, end - pos);
      pos = end + 2;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string value = line.substr(colon + 1);
      while (!value.empty() && value.front() == ' ') value.erase(value.begin());
      req.headers[Lower(line.substr(0, colon))] = value;
    }
  }
  size_t length = 0;
  try {
    const std::string cl = req.Header("content-length");
    length = cl.empty() ? 0 : static_cast<size_t>(std::stoul(cl));
  } catch (...) {
    length = 0;
  }
  if (length > 4 * 1024 * 1024) {
    netu::CloseSocket(c);
    return;
  }
  req.body = data.substr(head_end + 4);
  while (req.body.size() < length) {
    const int got = static_cast<int>(recv(c, buffer, sizeof(buffer), 0));
    if (got <= 0) {
      netu::CloseSocket(c);
      return;
    }
    req.body.append(buffer, static_cast<size_t>(got));
  }
  req.body.resize(length);

  Response res;
  try {
    std::lock_guard lock(state_->mutex);
    ++state_->requests;
    res = state_->Route(req);
  } catch (const HttpError& e) {
    res = JsonResponse(e.status, [&](Writer& w) {
      w.StartObject();
      w.Key("message"); w.String(e.message.c_str());
      w.Key("error"); w.String(ErrorName(e.status));
      w.Key("statusCode"); w.Int(e.status);
      w.EndObject();
    });
  } catch (...) {
    res = {500, R"({"message":"Internal server error","statusCode":500})", "application/json"};
  }

  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + Reason(res.status) + "\r\n";
  if (!res.content_type.empty()) out += "Content-Type: " + res.content_type + "\r\n";
  out += "Content-Length: " + std::to_string(res.body.size()) + "\r\nConnection: close\r\n\r\n";
  if (req.method != "HEAD") out += res.body;
  size_t sent = 0;
  while (sent < out.size()) {
    const int n = static_cast<int>(send(c, out.data() + sent, static_cast<int>(out.size() - sent), 0));
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
#if defined(_WIN32)
  shutdown(static_cast<SOCKET>(c), SD_SEND);
#else
  shutdown(c, SHUT_WR);
#endif
  netu::CloseSocket(c);
}

}  // namespace lobby
