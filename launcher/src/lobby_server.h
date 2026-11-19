// The LAN match server ("HOST" in PLAY ON LAN).
//
// A small Xenia Web Services server: the same REST calls and answers as the
// public Xenia Netplay server (github.com/AdrianCassar/Xenia-WebServices), for
// the parts the game uses. The game on every PC in the match is started in
// online mode with api_address = this PC, so the game's own online menu lists
// the matches hosted here. tools/netcheck/mock_server.py is the reference
// (same rules); `optroy_netcheck selftest` tests both.
//
// It also answers LAN scans (see lan_discovery.h), so JOIN lists this PC.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lobby {

inline constexpr uint16_t kDefaultPort = 36000;

struct Status {
  bool running = false;
  uint16_t port = 0;
  std::vector<std::string> players;  // gamertags signed in to this server
  int matches = 0;                   // sessions hosted here
  uint64_t requests = 0;
};

class Server {
 public:
  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Listens on every network interface. host_name is shown to PCs that scan
  // the LAN. False (with *error) when the port is in use etc.
  // discovery_port 0 = no LAN scan answers.
  bool Start(uint16_t port, const std::string& host_name, std::string* error,
             uint16_t discovery_port = 36001);
  void Stop();
  bool running() const;
  Status status() const;

  struct State;  // the server's data (players, sessions)

 private:
  void AcceptLoop();
  void DiscoveryLoop();
  void HandleConnection(uintptr_t socket, uint32_t peer_ip);

  std::unique_ptr<State> state_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> running_{false};
  uintptr_t listen_socket_ = ~uintptr_t(0);
  uintptr_t discovery_socket_ = ~uintptr_t(0);
  std::thread accept_thread_;
  std::thread discovery_thread_;
  std::atomic<int> open_connections_{0};
  uint16_t port_ = 0;
  uint16_t discovery_port_ = 0;
  std::string host_name_;
};

}  // namespace lobby
