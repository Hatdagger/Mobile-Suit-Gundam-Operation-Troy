/**
 * @file        kernel/xam/live/live_upnp.h
 * @brief       Opens the game's ports on the home router (UPnP IGD port mapping)
 *
 * With `upnp = true` and network_mode = "online", every port the game binds
 * is forwarded on the router, so other players can reach this PC directly.
 * Mappings are renewed while the game runs and removed when it exits.
 *
 * Self-contained (no miniupnpc): SSDP discovery over UDP multicast, the
 * router's description XML and SOAP calls over plain HTTP (rex/net/http.h).
 * Xenia Canary Netplay does the same with miniupnpc (src/xenia/kernel/upnp.cc).
 *
 * Settings: `upnp` (on/off), `upnp_root` (the router's description URL, to
 * skip discovery when the router doesn't answer it).
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/kernel/xam/live/live_api.h>

namespace rex::kernel::xam::live {

struct UpnpConfig {
  // The router's description URL (e.g. "http://192.168.1.1:5000/rootDesc.xml").
  // Empty: find the router with SSDP.
  std::string root_url;
  // How long to wait for routers to answer the SSDP search.
  uint32_t discover_ms = 2000;
  // SSDP target (tests use a local address instead of the multicast group).
  std::string ssdp_address = "239.255.255.250";
  uint16_t ssdp_port = 1900;
  // Shown in the router's port forwarding list.
  std::string description = "Gundam Operation Troy";
  // Mappings are asked for this long and renewed at half of it. Routers that
  // only allow permanent mappings get 0 (removed when the game exits).
  uint32_t lease_seconds = 3600;
  uint32_t http_timeout_ms = 3000;
};

// UPnP error codes worth naming (others are logged as numbers).
inline constexpr int kUpnpConflict = 718;           // port already forwarded to another device
inline constexpr int kUpnpOnlyPermanentLeases = 725;

class Upnp {
 public:
  explicit Upnp(UpnpConfig config, LogSink log = {});
  ~Upnp();  // calls Shutdown()

  Upnp(const Upnp&) = delete;
  Upnp& operator=(const Upnp&) = delete;

  // Background work (does not block the game).
  void OpenAsync(uint16_t port, const std::string& protocol);
  void CloseAsync(uint16_t port, const std::string& protocol);

  // Removes every mapping this object made and stops the background thread.
  void Shutdown();

  // --- Blocking calls (used by the background thread and by tests) ---

  // Finds the router's port mapping service. True when found (also when found before).
  bool Discover();
  // 0 on success, a UPnP error code from the router, or -1 when it didn't answer.
  int AddMapping(uint16_t port, const std::string& protocol);
  int DeleteMapping(uint16_t port, const std::string& protocol);
  // The router's public address, or "" when unknown.
  std::string ExternalIp();

  // Found router (after Discover): control URL and service type.
  std::string control_url() const;
  std::string service_type() const;
  // This PC's address on the router's network ("192.168.1.20").
  std::string local_ip() const;
  // Ports currently forwarded: {protocol, port}.
  std::vector<std::pair<std::string, uint16_t>> mapped() const;

  // Pieces exposed for tests.
  struct Service {
    std::string type;
    std::string control_url;  // absolute
  };
  // The best port mapping service in a description XML (empty type: none).
  static Service FindService(const std::string& description_xml, const std::string& location);
  // "LOCATION:" values from SSDP answers.
  static std::string LocationFromSsdp(const std::string& response);

 private:
  struct Job {
    bool open = true;
    uint16_t port = 0;
    std::string protocol;
  };

  void Run();
  void Log(LogLevel level, const std::string& message) const;
  bool DiscoverLocked();
  std::vector<std::string> SsdpSearch();
  bool TryLocation(const std::string& location);
  // Sends a SOAP action; fills out_body. Returns 0, a UPnP error code, or -1.
  int Soap(const std::string& action, const std::string& arguments, std::string* out_body);
  int AddMappingLease(uint16_t port, const std::string& protocol, uint32_t lease);
  void RenewAll();

  UpnpConfig config_;
  LogSink log_;

  mutable std::mutex mutex_;  // guards everything below
  std::condition_variable wake_;
  std::thread thread_;
  bool stopping_ = false;
  bool started_ = false;
  std::deque<Job> jobs_;

  bool discovered_ = false;
  bool discover_failed_ = false;
  std::chrono::steady_clock::time_point last_discover_attempt_{};
  Service service_;
  std::string local_ip_;
  bool permanent_only_ = false;
  // {protocol, port} -> how many game sockets use it.
  std::map<std::pair<std::string, uint16_t>, int> mapped_;
};

}  // namespace rex::kernel::xam::live
