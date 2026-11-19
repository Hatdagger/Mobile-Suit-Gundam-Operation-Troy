// optroy_netcheck - checks the recomp's online-play client without the game.
//
// It is built from the same source files the game uses for online play
// (thirdparty/rexglue-sdk: src/core/http_*.cpp, src/kernel/xam/live/live_api.cpp).
//
//   optroy_netcheck check [server]     Can this PC reach the server? (read only)
//   optroy_netcheck sessions [server]  Lists Operation Troy matches on the server (read only)
//   optroy_netcheck selftest <server>  Full create/search/join/leave test against a TEST
//                                      server such as tools/netcheck/mock_server.py.
//                                      Refuses the public server (it would leave test data).
//   optroy_netcheck sockets            Tests the Xbox socket translation on this PC (loopback).
//   optroy_netcheck upnp-selftest <description URL> <ssdp port> [<permanent-only URL>]
//                                      Tests router port opening against mock_igd.py.
//   optroy_netcheck upnp               Finds this PC's router and shows what UPnP allows
//                                      (opens and closes one test port).
//
// [server] defaults to the public Xenia Netplay server. Add --verbose for every request.

#include <rex/kernel/xam/live/live_api.h>
#include <rex/kernel/xam/live/live_identity.h>
#include <rex/kernel/xam/live/live_properties.h>
#include <rex/kernel/xam/live/live_upnp.h>
#include <rex/system/xsocket_native.h>
#include <rex/net/http.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <filesystem>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace rex::kernel::xam::live;

namespace {

constexpr uint32_t kTitleId = 0x424107D6;  // Gundam Operation Troy
bool g_verbose = false;
int g_failures = 0;

LogSink PrintSink() {
  return [](LogLevel level, const std::string& text) {
    if (level == LogLevel::kError || g_verbose) {
      std::printf("    [%s] %s\n",
                  level == LogLevel::kError     ? "error"
                  : level == LogLevel::kWarning ? "warning"
                  : level == LogLevel::kInfo    ? "info"
                                                : "debug",
                  text.c_str());
    }
  };
}

void Expect(bool ok, const std::string& what) {
  std::printf("  %s  %s\n", ok ? "OK  " : "FAIL", what.c_str());
  if (!ok) {
    ++g_failures;
  }
}

std::string Hex(uint64_t v) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(v));
  return buffer;
}

bool HasSession(const std::optional<std::vector<SessionInfo>>& list, uint64_t id) {
  if (!list) {
    return false;
  }
  for (const auto& s : *list) {
    if (s.session_id == id) {
      return true;
    }
  }
  return false;
}

// --- check ----------------------------------------------------------------------

int RunCheck(const std::string& server) {
  std::printf("HTTP backend: %s\n", rex::net::HttpBackendName().c_str());
  LiveApi api({.api_address = server, .title_id = kTitleId}, PrintSink());
  std::printf("Server: %s\n", api.api_address().c_str());

  const bool answers = api.Heartbeat();
  Expect(answers, "the server answers");
  if (!answers) {
    const CallResult r = api.last_result();
    std::printf("  (%s)\n", r.error.empty() ? ("HTTP " + std::to_string(r.status)).c_str()
                                            : r.error.c_str());
    return 1;
  }
  const auto ip = api.Whoami();
  Expect(ip.has_value(), "the server tells this PC its public address (whoami)");
  if (ip) {
    std::printf("  Public address: %s\n", MaskIp(*ip).c_str());
  }
  const auto ports = api.GetPortMappings();
  Expect(ports.has_value(), "Operation Troy port settings can be read");
  const uint32_t local = DetectLocalIp(HostFromServerAddress(api.api_address()));
  std::printf("  Local address used to reach it: %s\n",
              local ? MaskIp(Ipv4ToString(local)).c_str() : "(unknown)");
  return g_failures ? 1 : 0;
}

// --- sessions -------------------------------------------------------------------

int RunSessions(const std::string& server) {
  LiveApi api({.api_address = server, .title_id = kTitleId}, PrintSink());
  const auto sessions = api.GetTitleSessions();
  if (!sessions) {
    std::printf("Could not read the session list from %s\n", api.api_address().c_str());
    return 1;
  }
  std::printf("%zu Operation Troy session(s) on %s\n", sessions->size(), api.api_address().c_str());
  for (const auto& s : *sessions) {
    std::printf("  %s  host %s  players %u/%u  %s %s\n", Hex(s.session_id).c_str(),
                MaskIp(s.host_address).c_str(), s.filled_public_slots + s.filled_private_slots,
                s.public_slots + s.private_slots, s.title.c_str(), s.version.c_str());
  }
  return 0;
}

// --- selftest -------------------------------------------------------------------

void UnitChecks() {
  std::printf("Helpers\n");
  Expect(NormalizeServerAddress("192.168.1.20:36000") == "http://192.168.1.20:36000/",
         "LAN address without http:// gets http:// and a slash");
  Expect(NormalizeServerAddress(" https://example.com ") == "https://example.com/",
         "https address is kept, spaces trimmed");
  Expect(ParseNetworkMode("Online") == NetworkMode::kOnline && ParseNetworkMode("2") ==
                                                                   NetworkMode::kOnline,
         "network_mode accepts online / 2");
  Expect(ParseNetworkMode("offline") == NetworkMode::kOffline && !ParseNetworkMode("maybe"),
         "network_mode accepts offline, rejects unknown words");
  Expect(MacToString(0x02AABBCC0001) == "02AABBCC0001", "MAC to text");
  Expect(MacFromString("02:aa:bb:cc:00:01") == 0x02AABBCC0001ull, "MAC from text");
  Expect(MachineIdFromMac(0x02AABBCC0001) == 0xFA0002AABBCC0001ull, "machine ID from MAC");
  Expect(MaskIp("203.0.113.42") == "203.0.113.x", "IP masking for logs");

  std::printf("Identity and addresses\n");
  const Identity made = GenerateIdentity();
  Expect((made.mac_address >> 24) == 0x7C1E52 && (made.online_xuid >> 48) == 0x9,
         "new identity: Xbox 360 MAC prefix, online XUID 0009...");
  const auto parsed = ParseIdentity(FormatIdentity(made));
  Expect(parsed && parsed->mac_address == made.mac_address &&
             parsed->online_xuid == made.online_xuid,
         "identity file text reads back the same");
  Expect(!ParseIdentity("mac_address = \"12\"\n") && !ParseIdentity(""),
         "a broken identity file is rejected");
  const auto dir = std::filesystem::temp_directory_path() / "optroy_netcheck_identity";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  const Identity first = LoadOrCreateIdentity(dir);
  const Identity again = LoadOrCreateIdentity(dir);
  Expect(std::filesystem::exists(dir / kIdentityFileName) &&
             first.mac_address == again.mac_address && first.online_xuid == again.online_xuid,
         "identity is saved once and then stays the same");
  std::filesystem::remove_all(dir, ec);
  Expect(SanitizeGamertag("  Amuro Ray the Newtype  ") == "Amuro Ray the N" &&
             SanitizeGamertag("") == "Player" && SanitizeGamertag("\t\n") == "Player",
         "gamertag: trimmed, 15 characters at most, \"Player\" if empty");
  Expect(HostFromServerAddress("https://example.com/") == "example.com" &&
             HostFromServerAddress("192.168.1.20:36000") == "192.168.1.20" &&
             HostFromServerAddress("http://10.0.0.5:36000/x") == "10.0.0.5",
         "host name from a server address");
  Expect(Ipv4ToString(ParseIpv4("192.168.1.20")) == "192.168.1.20" && !ParseIpv4("1.2.3") &&
             !ParseIpv4("256.1.1.1") && !ParseIpv4("1.2.3.4x"),
         "IPv4 text to address and back");
  Expect(IsLoopbackIp(ParseIpv4("127.0.0.1")) && !IsLoopbackIp(ParseIpv4("192.168.1.1")),
         "loopback address check");
  const uint32_t local = DetectLocalIp("1.1.1.1");
  Expect(local != 0 && !IsLoopbackIp(local),
         "this PC's local address is found: " + Ipv4ToString(local));
}

int RunSelftest(const std::string& server) {
  if (NormalizeServerAddress(server) == NormalizeServerAddress(kPublicServer)) {
    std::printf("selftest writes test players and sessions, so it does not run against the\n"
                "public server. Start a test server (python3 tools/netcheck/mock_server.py)\n"
                "and pass its address, e.g. http://127.0.0.1:36000\n");
    return 2;
  }

  UnitChecks();

  std::printf("\nServer %s (HTTP backend: %s)\n", NormalizeServerAddress(server).c_str(),
              rex::net::HttpBackendName().c_str());
  LiveApi api({.api_address = server, .title_id = kTitleId, .mask_ips_in_log = false},
              PrintSink());

  std::random_device rd;
  const uint64_t session_id =
      0xAE00000000000000ull | (((uint64_t(rd()) << 24) ^ rd()) & 0xFFFFFFFFFFFFull);
  const uint64_t host_xuid = 0x0009000000000000ull | (rd() & 0xFFFFFF);
  const uint64_t guest_xuid = host_xuid + 1;
  const uint64_t host_mac = 0x02AABB000000ull | (rd() & 0xFFFFFF);
  const uint64_t guest_mac = host_mac ^ 1;
  constexpr uint32_t kFlags = 0x2F;  // host, presence, stats, matchmaking, peer network

  Expect(api.Heartbeat(), "server answers (heartbeat)");
  Expect(api.Connect(NetworkMode::kOnline), "connect in online mode (whoami)");
  const std::string ip = api.online_ip();
  Expect(!ip.empty(), "public address known: " + ip);
  Expect(api.GetPortMappings().has_value(), "port mappings for the title");
  // Players are found by address, so remove earlier test players from this address first.
  Expect(api.DeleteMyProfiles().has_value(), "remove players left from earlier tests");

  std::printf("Players\n");
  PlayerInfo host{.xuid = host_xuid, .gamertag = "TestHost", .machine_id = MachineIdFromMac(host_mac),
                  .host_address = ip, .mac_address = host_mac};
  host.settings[0xFFFE07D1] = {"AAAAAA=="};  // opaque to the client; just passed along
  Expect(api.RegisterPlayer(host), "register host player");
  const auto found = api.FindPlayer(ip);
  Expect(found && found->xuid == host_xuid && found->gamertag == "TestHost",
         "find the host player by address");
  Expect(found && found->mac_address == host_mac, "found player has the right MAC");
  PlayerInfo guest{.xuid = guest_xuid, .gamertag = "TestGuest",
                   .machine_id = MachineIdFromMac(guest_mac), .host_address = ip,
                   .mac_address = guest_mac};
  Expect(api.RegisterPlayer(guest), "register guest player");

  std::printf("Session %s\n", Hex(session_id).c_str());
  SessionInfo session{.session_id = session_id, .host_xuid = host_xuid,
                      .title = "Gundam Operation Troy", .media_id = "12345678",
                      .version = "1.0.0.0", .flags = kFlags, .public_slots = 9, .private_slots = 1,
                      .host_address = ip, .mac_address = host_mac, .port = kPlayerPort};
  Expect(api.CreateSession(session), "create session");
  const auto got = api.GetSession(session_id);
  Expect(got && got->session_id == session_id && got->host_address == ip &&
             got->mac_address == host_mac && got->port == kPlayerPort,
         "read the session back (id, host address, MAC, port)");
  Expect(got && got->public_slots == 9 && got->private_slots == 1 && got->open_public_slots == 9,
         "session slots");

  SessionSearchQuery query{.search_index = 0, .results_count = 10, .num_users = 1,
                           .searcher_xuid = guest_xuid};
  Expect(!HasSession(api.SearchSessions(query), session_id),
         "session without properties is not listed yet");

  std::printf("Session properties\n");
  {
    const Property mode = Property::Context(kContextGameMode, 1);
    const auto round = Property::FromBase64(mode.ToBase64());
    Expect(round && *round == mode && round->u32() == 1, "context property survives base64");
    const Property name = Property::WString(kPropertyGamerHostname, "TestHost");
    const auto name_round = Property::FromBase64(name.ToBase64());
    Expect(name_round && *name_round == name && name_round->extra.size() == 18,
           "string property survives base64 (UTF-16 with terminator)");
  }
  Expect(api.SetSessionProperties(
             session_id, {Property::Context(kContextGameMode, 1).ToBase64(),
                          Property::Context(kContextGameType, 0).ToBase64(),
                          Property::Int64(kPropertyGamerPuid, int64_t(host_xuid)).ToBase64(),
                          Property::WString(kPropertyGamerHostname, "TestHost").ToBase64()}),
         "host uploads session properties and contexts");
  query.filters = {Property::Context(kContextGameMode, 1).ToBase64(),
                   Property::Context(kContextGameType, 0).ToBase64()};
  Expect(HasSession(api.SearchSessions(query), session_id),
         "guest finds the session by search (matching game mode)");
  query.filters[0] = Property::Context(kContextGameMode, 2).ToBase64();
  Expect(!HasSession(api.SearchSessions(query), session_id),
         "search for another game mode does not find it");
  query.filters[0] = Property::Context(kContextGameMode, 1).ToBase64();
  {
    const auto props = api.GetSessionProperties(session_id, 0);
    bool has_name = false;
    for (const auto& text : props.value_or(std::vector<std::string>{})) {
      const auto p = Property::FromBase64(text);
      has_name |= p && p->id == kPropertyGamerHostname;
    }
    Expect(props && props->size() == 2 && has_name,
           "search result properties come back (no contexts, host name included)");
  }
  query.searcher_xuid = host_xuid;
  const auto own = api.SearchSessions(query);
  Expect(own.has_value() && !HasSession(own, session_id), "host's own session is not in its search");
  Expect(HasSession(api.GetTitleSessions(), session_id), "session is in the title's session list");

  Expect(api.JoinSession(session_id, {{host_xuid, false}, {guest_xuid, true}}),
         "host and guest join (guest in the private slot)");
  Expect(api.PreJoinSession(session_id, {guest_xuid}), "pre-join");
  auto details = api.GetSessionDetails(session_id);
  Expect(details && details->player_xuids.size() == 2, "details list 2 players");
  Expect(details && details->filled_public_slots == 1 && details->filled_private_slots == 1 &&
             details->open_public_slots == 8 && details->open_private_slots == 0,
         "slot counts after joining");
  const auto arbitration = api.GetArbitration(session_id);
  Expect(arbitration && arbitration->total_players == 2 && arbitration->machines.size() == 2,
         "arbitration: 2 players on 2 machines");

  const std::vector<uint8_t> qos = {0x00, 0x01, 0xFF, 0x7F, 0x00, 0x42, 0x0D, 0x0A, 0x26, 0x3D};
  Expect(api.PostQos(session_id, qos), "publish QoS data (binary)");
  const auto qos_back = api.GetQos(session_id);
  Expect(qos_back && *qos_back == qos, "QoS data comes back byte for byte");
  const auto no_qos = api.GetQos(session_id ^ 0xFFFF);
  Expect(no_qos && no_qos->empty(), "no QoS data for an unknown session");

  Expect(api.ModifySession(session_id, kFlags, 5, 1), "modify session (5 public slots)");
  details = api.GetSessionDetails(session_id);
  Expect(details && details->public_slots == 5, "new slot count");

  Expect(api.LeaveSession(session_id, {guest_xuid}), "guest leaves");
  details = api.GetSessionDetails(session_id);
  Expect(details && details->player_xuids.size() == 1, "1 player left");

  const auto migrated = api.MigrateSession(session_id, guest_xuid, ip, guest_mac, kPlayerPort);
  Expect(migrated && migrated->session_id != 0 && migrated->session_id != session_id &&
             migrated->mac_address == guest_mac,
         "host migration gives a new session with the new host");
  Expect(!api.GetSession(session_id), "old session is gone after migration");
  const CallResult missing = api.last_result();
  Expect(missing.status == 404 && !missing.message.empty(),
         "server explains a missing session (HTTP 404 + message)");

  const uint64_t final_id = migrated ? migrated->session_id : session_id;
  Expect(api.DeleteSession(final_id), "delete session");
  Expect(!api.GetSession(final_id), "session is gone after delete");
  Expect(api.DeleteMySessions(host_mac), "delete this machine's sessions (by MAC)");
  const auto removed = api.DeleteMyProfiles();
  bool removed_both = false;
  if (removed) {
    int count = 0;
    for (const auto& [gamertag, xuid] : *removed) {
      count += (xuid == host_xuid && gamertag == "TestHost") ||
               (xuid == guest_xuid && gamertag == "TestGuest");
    }
    removed_both = count == 2;
  }
  Expect(removed_both, "remove the test players again");

  std::printf("Errors\n");
  LiveApi nowhere({.api_address = "127.0.0.1:1", .title_id = kTitleId, .timeout_ms = 3000,
                   .connect_timeout_ms = 2000});
  Expect(!nowhere.Heartbeat() && !nowhere.Whoami(), "an unreachable server fails cleanly");
  Expect(!nowhere.last_result().error.empty(), "with a reason: " + nowhere.last_result().error);

  std::printf("\n%s (%d failed)\n", g_failures ? "SELFTEST FAILED" : "SELFTEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}


// --- sockets --------------------------------------------------------------------

namespace sock = rex::system::sock;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

constexpr uint32_t kLoopback = 0x7F000001;  // 127.0.0.1 as the game sees it

int RunSockets() {
  std::printf("UDP\n");
  sock::Handle a = sock::Open(2, sock::kXSockDgram, 17);
  sock::Handle b = sock::Open(2, sock::kXSockDgram, 254);  // VDP becomes UDP
  Expect(a != sock::kInvalidHandle && b != sock::kInvalidHandle, "open UDP and VDP sockets");
  Expect(sock::Bind(a, {kLoopback, 0}) == 0, "bind to 127.0.0.1, any port");
  sock::Endpoint name_a;
  Expect(sock::GetSockName(a, &name_a) == 0 && name_a.ip == kLoopback && name_a.port != 0,
         "getsockname gives 127.0.0.1 and a port (not byte-swapped)");
  Expect(sock::Bind(b, {0, 0}) == 0, "bind second socket to any address");

  const char hello[] = "hello xbox";
  Expect(sock::SendTo(b, hello, sizeof(hello), 0, &name_a) == int(sizeof(hello)),
         "sendto reaches the address and port given as numbers");
  sock::Endpoint name_b;
  sock::GetSockName(b, &name_b);
  char buffer[64] = {};
  sock::Endpoint from;
  int got = sock::RecvFrom(a, buffer, sizeof(buffer), 0, &from);
  Expect(got == int(sizeof(hello)) && std::memcmp(buffer, hello, sizeof(hello)) == 0,
         "recvfrom gets the data");
  Expect(from.ip == kLoopback && from.port == name_b.port,
         "recvfrom reports the sender's address and port correctly");

  uint8_t on[4];
  PutBe32(on, 1);
  Expect(sock::IoControl(a, sock::kXFionbio, on) == 0, "FIONBIO (big-endian 1) makes it non-blocking");
  got = sock::RecvFrom(a, buffer, sizeof(buffer), 0, &from);
  Expect(got == -1 && sock::LastError() == sock::kWsaEWOULDBLOCK,
         "empty non-blocking receive fails with WSAEWOULDBLOCK (10035), got " +
             std::to_string(sock::LastError()));

  sock::SendTo(b, hello, sizeof(hello), 0, &name_a);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  uint8_t available[4] = {};
  const bool fionread_ok = sock::IoControl(a, sock::kXFionread, available) == 0;
  Expect(fionread_ok && Be32(available) >= sizeof(hello),
         "FIONREAD answers big-endian (" + std::to_string(Be32(available)) + " bytes)");
  sock::RecvFrom(a, buffer, sizeof(buffer), 0, nullptr);

  uint8_t value[4];
  uint32_t len = 4;
  PutBe32(value, 65536);
  Expect(sock::SetOption(a, sock::kXSolSocket, 0x1002, value, 4) == 0,
         "SO_RCVBUF set with a big-endian value");
  std::memset(value, 0, 4);
  const bool rcvbuf_ok = sock::GetOption(a, sock::kXSolSocket, 0x1002, value, &len) == 0;
  Expect(rcvbuf_ok && len == 4 && Be32(value) >= 65536 && Be32(value) < 16 * 1024 * 1024,
         "SO_RCVBUF reads back big-endian (" + std::to_string(Be32(value)) + ")");
  len = 4;
  Expect(sock::GetOption(a, sock::kXSolSocket, 0x1008, value, &len) == 0 && Be32(value) == 2,
         "SO_TYPE reads 2 (datagram)");
  PutBe32(value, 1);
  Expect(sock::SetOption(a, sock::kXSolSocket, sock::kXSoBroadcast, value, 4) == 0,
         "SO_BROADCAST on");
  len = 4;
  Expect(sock::GetOption(a, sock::kXSolSocket, sock::kXSoBroadcast, value, &len) == 0 &&
             Be32(value) != 0,
         "SO_BROADCAST reads back on");
  uint8_t small[1] = {};
  len = 1;
  Expect(sock::GetOption(a, sock::kXSolSocket, 0x1008, small, &len) == 0 && len == 1 &&
             small[0] == 2,
         "a 1-byte buffer gets the low byte");
  PutBe32(value, 1500);
  Expect(sock::SetOption(a, sock::kXSolSocket, 0x1006, value, 4) == 0, "SO_RCVTIMEO 1500 ms");
  len = 4;
  const bool timeo_ok = sock::GetOption(a, sock::kXSolSocket, 0x1006, value, &len) == 0;
  Expect(timeo_ok && Be32(value) >= 1500 && Be32(value) <= 1510,
         "SO_RCVTIMEO reads back 1500 ms (" + std::to_string(Be32(value)) + ")");
  Expect(sock::SetOption(a, sock::kXSolSocket, sock::kXSoMarkInsecure, value, 4) == 0,
         "Xbox security option accepted");
  Expect(sock::SetOption(a, sock::kXSolSocket, 0x4242, value, 4) == -1 &&
             sock::LastError() == sock::kWsaENOPROTOOPT,
         "unknown option fails with WSAENOPROTOOPT");
  PutBe32(value, 1);
  Expect(sock::SetOption(a, sock::kXSolSocket, ~sock::kXSoReuseAddr, value, 4) == 0,
         "SO_EXCLUSIVEADDRUSE accepted");

  sock::Close(a);
  sock::Close(b);

  std::printf("TCP\n");
  sock::Handle server = sock::Open(2, sock::kXSockStream, 6);
  sock::Handle client = sock::Open(2, sock::kXSockStream, 6);
  Expect(sock::Bind(server, {kLoopback, 0}) == 0 && sock::Listen(server, 4) == 0, "listen");
  sock::Endpoint server_name;
  sock::GetSockName(server, &server_name);
  PutBe32(on, 1);
  sock::IoControl(client, sock::kXFionbio, on);
  int c = sock::Connect(client, server_name);
  Expect(c == 0 || sock::LastError() == sock::kWsaEWOULDBLOCK,
         "non-blocking connect returns 0 or WSAEWOULDBLOCK (got " + std::to_string(sock::LastError()) +
             ")");
  sock::Endpoint peer;
  sock::Handle accepted = sock::Accept(server, &peer);
  Expect(accepted != sock::kInvalidHandle && peer.ip == kLoopback, "accept reports 127.0.0.1");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  len = 4;
  Expect(sock::GetOption(client, sock::kXSolSocket, 0x1007, value, &len) == 0 && Be32(value) == 0,
         "SO_ERROR is 0 after connecting");
  sock::Endpoint client_peer;
  Expect(sock::GetPeerName(client, &client_peer) == 0 && client_peer.port == server_name.port,
         "getpeername gives the server's port");
  uint8_t linger_value[4] = {0, 1, 0, 5};
  Expect(sock::SetOption(client, sock::kXSolSocket, sock::kXSoLinger, linger_value, 4) == 0,
         "SO_LINGER (two big-endian u16) set");
  uint8_t linger_back[4] = {};
  len = 4;
  Expect(sock::GetOption(client, sock::kXSolSocket, sock::kXSoLinger, linger_back, &len) == 0 &&
             linger_back[1] == 1 && linger_back[3] == 5,
         "SO_LINGER reads back on, 5 s");
  PutBe32(value, 1);
  Expect(sock::SetOption(client, sock::kXIpprotoTcp, 1, value, 4) == 0, "TCP_NODELAY on");
  Expect(sock::Send(client, hello, sizeof(hello), 0) == int(sizeof(hello)), "send");
  std::memset(buffer, 0, sizeof(buffer));
  got = sock::Recv(accepted, buffer, sizeof(buffer), 0);
  Expect(got == int(sizeof(hello)) && std::strcmp(buffer, hello) == 0, "recv");
  sock::Close(accepted);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Sending to a closed peer must fail with an error, not end the program (SIGPIPE).
  sock::Send(client, hello, sizeof(hello), 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  int dead = sock::Send(client, hello, sizeof(hello), 0);
  Expect(dead == -1 || dead == int(sizeof(hello)), "send to a closed peer doesn't crash");
  sock::Close(client);
  sock::Close(server);
  Expect(sock::Close(server) == -1, "closing twice fails cleanly");

  std::printf("%s\n", g_failures ? "SOME CHECKS FAILED" : "All socket checks passed.");
  return g_failures ? 1 : 0;
}

// --- upnp -----------------------------------------------------------------------

// Mappings on the mock router: "UDP:1000" entries from GET /mappings.
std::string MockMappings(const std::string& description_url) {
  rex::net::HttpRequest request;
  request.url = description_url.substr(0, description_url.rfind('/')) + "/mappings";
  return rex::net::HttpSend(request).body;
}

bool WaitFor(const std::function<bool()>& condition) {
  for (int i = 0; i < 60; ++i) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool HasMapping(const std::string& listing, const std::string& protocol, int port) {
  return listing.find("\"protocol\": \"" + protocol + "\", \"port\": " + std::to_string(port) +
                      ",") != std::string::npos;
}

int RunUpnpSelftest(const std::string& root, uint16_t ssdp_port, const std::string& permanent_root) {
  std::printf("Parsing\n");
  Expect(Upnp::LocationFromSsdp("HTTP/1.1 200 OK\r\nST: x\r\nlocation: http://10.0.0.1:1900/a.xml "
                                "\r\n\r\n") == "http://10.0.0.1:1900/a.xml",
         "LOCATION header found in an SSDP answer (any case)");
  const auto service = Upnp::FindService(
      "<root><URLBase>http://10.0.0.1:5000/</URLBase><device><serviceList><service>"
      "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
      "<controlURL>/ppp</controlURL></service><service>"
      "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
      "<controlURL>/ip</controlURL></service></serviceList></device></root>",
      "http://10.0.0.1:5000/desc.xml");
  Expect(service.type == "urn:schemas-upnp-org:service:WANIPConnection:1" &&
             service.control_url == "http://10.0.0.1:5000/ip",
         "WANIPConnection preferred over WANPPPConnection; URLBase used");

  std::printf("Router at a known address (upnp_root)\n");
  {
    UpnpConfig config;
    config.root_url = root;
    Upnp upnp(config, PrintSink());
    Expect(upnp.Discover(), "router found");
    Expect(upnp.control_url().ends_with("/ctl/IPConn"), "relative control URL resolved: " +
                                                            upnp.control_url());
    Expect(!upnp.local_ip().empty(), "this PC's address known: " + MaskIp(upnp.local_ip()));
    Expect(upnp.ExternalIp() == "203.0.113.7", "router's public address read");
    Expect(upnp.AddMapping(1000, "UDP") == 0, "open UDP port 1000");
    const std::string listing = MockMappings(root);
    Expect(HasMapping(listing, "UDP", 1000) && listing.find("\"lease\": 3600") != std::string::npos,
           "router lists UDP 1000 with a 1 hour lease");
    Expect(upnp.AddMapping(7777, "UDP") == kUpnpConflict,
           "a port used by another device is reported (718)");
    Expect(upnp.DeleteMapping(1000, "UDP") == 0, "close UDP port 1000");
    Expect(upnp.DeleteMapping(1000, "UDP") == 714, "closing it again: router says no such entry");
  }

  std::printf("Router found by SSDP, ports opened in the background\n");
  {
    UpnpConfig config;
    config.ssdp_address = "127.0.0.1";
    config.ssdp_port = ssdp_port;
    config.discover_ms = 800;
    Upnp upnp(config, PrintSink());
    Expect(upnp.Discover(), "router answers the SSDP search");
    upnp.OpenAsync(1000, "UDP");
    upnp.OpenAsync(1000, "UDP");  // two game sockets, same port
    upnp.OpenAsync(3075, "TCP");
    Expect(WaitFor([&] {
             const std::string l = MockMappings(root);
             return HasMapping(l, "UDP", 1000) && HasMapping(l, "TCP", 3075);
           }),
           "UDP 1000 and TCP 3075 opened in the background");
    upnp.CloseAsync(1000, "UDP");
    upnp.CloseAsync(3075, "TCP");
    Expect(WaitFor([&] { return !HasMapping(MockMappings(root), "TCP", 3075); }),
           "TCP 3075 closed with its socket");
    Expect(HasMapping(MockMappings(root), "UDP", 1000),
           "UDP 1000 stays open while the second socket uses it");
    upnp.Shutdown();
    Expect(!HasMapping(MockMappings(root), "UDP", 1000), "everything closed when the game exits");
  }

  if (!permanent_root.empty()) {
    std::printf("Router that only allows permanent mappings\n");
    UpnpConfig config;
    config.root_url = permanent_root;
    Upnp upnp(config, PrintSink());
    Expect(upnp.AddMapping(1001, "UDP") == 0, "port opened after retrying without a lease (725)");
    Expect(MockMappings(permanent_root).find("\"lease\": 0") != std::string::npos,
           "router lists it as permanent");
    upnp.Shutdown();
    Expect(upnp.mapped().empty() && !HasMapping(MockMappings(permanent_root), "UDP", 1001),
           "removed again on exit");
  }

  std::printf("%s\n", g_failures ? "SOME CHECKS FAILED" : "All UPnP checks passed.");
  return g_failures ? 1 : 0;
}

int RunUpnp() {
  g_verbose = true;
  Upnp upnp(UpnpConfig{}, PrintSink());
  if (!upnp.Discover()) {
    std::printf("No router with UPnP found. Players may still connect if ports are forwarded by hand.\n");
    return 1;
  }
  std::printf("Router: %s\nThis PC: %s\nPublic address: %s\n", upnp.service_type().c_str(),
              MaskIp(upnp.local_ip()).c_str(), MaskIp(upnp.ExternalIp()).c_str());
  const int added = upnp.AddMapping(36099, "UDP");
  Expect(added == 0, "the router lets this PC open a port (test port UDP 36099)");
  if (added == 0) {
    Expect(upnp.DeleteMapping(36099, "UDP") == 0, "and close it again");
  }
  return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
      g_verbose = true;
    } else {
      args.emplace_back(argv[i]);
    }
  }
  const std::string command = args.empty() ? "check" : args[0];
  const std::string server = args.size() > 1 ? args[1] : std::string(kPublicServer);

  if (command == "check") {
    return RunCheck(server);
  }
  if (command == "sessions") {
    return RunSessions(server);
  }
  if (command == "selftest") {
    if (args.size() < 2) {
      std::printf("selftest needs a test server address, e.g. http://127.0.0.1:36000\n");
      return 2;
    }
    return RunSelftest(server);
  }
  if (command == "sockets") {
    return RunSockets();
  }
  if (command == "upnp") {
    return RunUpnp();
  }
  if (command == "upnp-selftest") {
    if (args.size() < 3) {
      std::printf("upnp-selftest needs the mock router's description URL and SSDP port, e.g.\n"
                  "  upnp-selftest http://127.0.0.1:5000/rootDesc.xml 1901\n");
      return 2;
    }
    return RunUpnpSelftest(args[1], static_cast<uint16_t>(std::stoi(args[2])),
                           args.size() > 3 ? args[3] : std::string());
  }
  std::printf("Usage: optroy_netcheck check|sessions|selftest|sockets|upnp|upnp-selftest "
              "[server] [--verbose]\n");
  return 2;
}
