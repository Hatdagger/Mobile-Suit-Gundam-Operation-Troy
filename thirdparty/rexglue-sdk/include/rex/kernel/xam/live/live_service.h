/**
 * @file        kernel/xam/live/live_service.h
 * @brief       The running Xbox Live replacement: identity, connection, sign-in state
 *
 * One LiveService exists for the whole program. Runtime starts it when the
 * game is launched and stops it on shutdown. It
 *  - keeps this PC's network identity (MAC address and online XUID) in
 *    <user data>/live_identity.toml, created on first use, so other players
 *    and the server always see the same console;
 *  - in online mode, connects to the server in api_address in the background
 *    (whoami, then registers the player), retries while the server cannot be
 *    reached, and tells the game when the connection comes or goes;
 *  - answers the questions the kernel asks (signed in to Live? our addresses?).
 *
 * The connection work follows Xenia Canary Netplay's XLiveAPI::Init and its
 * periodic maintenance (src/xenia/kernel/XLiveAPI.cpp, xam/user_tracker.cc).
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/kernel/xam/live/live_api.h>
#include <rex/kernel/xam/live/live_identity.h>
#include <rex/kernel/xam/live/live_upnp.h>

namespace rex::kernel::xam::live {

enum class ConnectionState {
  kOffline,     // network_mode = offline
  kConnecting,  // first attempt still running
  kConnected,   // server reachable (online: public address known; player registered)
  kFailed,      // server not reachable; retrying in the background
};

class LiveService {
 public:
  static LiveService& Get();

  // At game launch. title_id: the game's; user_data_dir: where live_identity.toml lives.
  void Start(uint32_t title_id, const std::filesystem::path& user_data_dir);
  // At shutdown: removes this PC's sessions from the server, stops the background thread.
  void Stop();

  bool started() const { return started_.load(); }
  NetworkMode mode() const { return mode_; }
  ConnectionState state() const { return state_.load(); }

  // Waits for the first connection attempt (at most ~10 seconds), then tells whether
  // the profile is signed in to Live: online mode and the server reachable.
  bool SignedInToLive();
  // Waits like SignedInToLive; true when the server can be used (online or lan mode).
  bool Connected();

  // This PC's identity (loaded on first use, also before Start).
  const Identity& identity();
  uint64_t machine_id() { return MachineIdFromMac(identity().mac_address); }
  // The gamertag setting, cleaned up (see SanitizeGamertag).
  std::string gamertag() const;
  // IPv4 addresses in network byte order (as in in_addr.s_addr); 0 when unknown.
  uint32_t local_ip() const { return local_ip_.load(); }
  uint32_t online_ip() const { return online_ip_.load(); }
  uint16_t player_port() const { return kPlayerPort; }

  // The server client; null in offline mode or before Start. Shared, so background
  // work can keep using it while the program shuts down.
  std::shared_ptr<LiveApi> api() const { return api_; }

  // XNetInAddrToXnAddr: MAC and session of the player at a public IPv4 (network
  // order), asking the server once and then remembering the answer.
  struct RemotePlayer {
    uint64_t mac_address = 0;
    uint64_t session_id = 0;
  };
  RemotePlayer LookupRemote(uint32_t ip);
  void ForgetRemotes();

  // XNetQosListen: publishes QoS data for a session in the background when it changed.
  void PublishQos(uint64_t session_id, std::vector<uint8_t> data);
  void ForgetQos(uint64_t session_id);

 private:
  LiveService() = default;
  ~LiveService();

  void Run();
  bool ConnectOnce();
  void Broadcast(bool connected);
  void WaitForFirstAttempt();

  // UPnP: open the ports game sockets bind (online mode with upnp = true).
  void StartUpnp();
  void StopUpnp();
  std::unique_ptr<Upnp> upnp_;

  std::atomic<bool> started_{false};
  NetworkMode mode_ = NetworkMode::kOffline;
  uint32_t title_id_ = 0;
  std::filesystem::path user_data_dir_;
  std::once_flag identity_once_;
  Identity identity_;
  std::shared_ptr<LiveApi> api_;

  std::atomic<ConnectionState> state_{ConnectionState::kOffline};
  std::atomic<uint32_t> local_ip_{0};
  std::atomic<uint32_t> online_ip_{0};

  std::mutex mutex_;
  std::condition_variable wake_;
  bool first_attempt_done_ = false;
  bool waited_too_long_ = false;  // a caller gave up waiting for the first attempt
  bool stopping_ = false;
  std::thread thread_;

  std::mutex cache_mutex_;
  std::map<uint32_t, RemotePlayer> remotes_;
  std::map<uint64_t, std::vector<uint8_t>> qos_published_;
};

}  // namespace rex::kernel::xam::live
