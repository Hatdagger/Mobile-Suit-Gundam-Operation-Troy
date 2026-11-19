// Finding LAN hosts: JOIN sends a small UDP broadcast, and every PC hosting a
// match (lobby::Server) answers with its name and port.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lan {

inline constexpr uint16_t kDiscoveryPort = 36001;
inline constexpr const char* kQuery = "OPTROY_LAN_QUERY 1\n";
inline constexpr const char* kReplyHeader = "OPTROY_LAN_HOST 1\n";

struct HostInfo {
  std::string name;     // host's player name
  std::string address;  // IPv4 the answer came from
  uint16_t port = 0;    // match server port (36000)
  int players = 0;
  int matches = 0;
  std::string version;  // launcher version of the host
  std::chrono::steady_clock::time_point seen{};
};

// The answer a host sends; and reading one (address filled in by the caller).
std::string FormatReply(const HostInfo& info);
std::optional<HostInfo> ParseReply(const std::string& text);

// Scans the network while running: asks every second, keeps hosts seen in the
// last 5 seconds. target: where to ask (empty = every interface's broadcast
// address plus 255.255.255.255); tests use "127.0.0.1".
class Scanner {
 public:
  ~Scanner();
  void Start(uint16_t discovery_port = kDiscoveryPort, std::string target = {});
  void Stop();
  bool running() const { return running_.load(); }
  std::vector<HostInfo> hosts() const;

 private:
  void Run();

  uint16_t discovery_port_ = kDiscoveryPort;
  std::string target_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<HostInfo> hosts_;
};

}  // namespace lan
