/**
 * @file        kernel/xam/live/live_service.cpp
 * @brief       The running Xbox Live replacement (see live_service.h)
 *
 * Connection flow after Xenia Canary Netplay's XLiveAPI::Init /
 * RefreshNetworkMode (XLiveAPI.cpp) and UserTracker periodic maintenance
 * (xam/user_tracker.cc): whoami, delete this console's old sessions, register
 * the player; then heartbeat now and then, and tell the game when the
 * connection comes or goes.
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */

#include <rex/kernel/xam/live/live_service.h>

#include <rex/kernel/xam/live/live_settings.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <chrono>

namespace rex::kernel::xam::live {

namespace {

using namespace std::chrono_literals;

// Notification IDs and values (Xenia's xbox.h / xnet.h).
constexpr uint32_t kXNotificationSystemSignInChanged = 0x0000000A;
constexpr uint32_t kXNotificationLiveConnectionChanged = 0x02000001;
constexpr uint32_t kXNotificationLiveLinkStateChanged = 0x02000003;
constexpr uint32_t X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED = 0x001510F0;
constexpr uint32_t X_ONLINE_S_LOGON_DISCONNECTED = 0x001510F1;

// How long a game thread waits for the first connection attempt.
constexpr auto kFirstAttemptWait = 12s;
// Background checks.
constexpr auto kRetryInterval = 15s;
constexpr auto kHeartbeatInterval = 30s;

std::string ShowIp(uint32_t ip) {
  const std::string text = Ipv4ToString(ip);
  return REXCVAR_GET(log_mask_ips) ? MaskIp(text) : text;
}

}  // namespace

LiveService& LiveService::Get() {
  static LiveService service;
  return service;
}

LiveService::~LiveService() {
  Stop();
}

const Identity& LiveService::identity() {
  std::call_once(identity_once_, [this] {
    std::filesystem::path dir = user_data_dir_;
    if (dir.empty() && rex::Runtime::instance()) {
      dir = rex::Runtime::instance()->user_data_root();
    }
    if (dir.empty()) {
      identity_ = GenerateIdentity();  // nowhere to keep it: this run only
      REXKRNL_WARN("Live: no user data folder; using a temporary network identity");
    } else {
      identity_ = LoadOrCreateIdentity(dir);
    }
    REXKRNL_INFO("Live: console MAC {}, online XUID {:016X}", MacToString(identity_.mac_address),
                 identity_.online_xuid);
  });
  return identity_;
}

std::string LiveService::gamertag() const {
  return SanitizeGamertag(REXCVAR_GET(gamertag));
}

void LiveService::Start(uint32_t title_id, const std::filesystem::path& user_data_dir) {
  if (started_.exchange(true)) {
    return;
  }
  title_id_ = title_id;
  user_data_dir_ = user_data_dir;
  mode_ = CurrentNetworkMode();
  identity();

  if (mode_ == NetworkMode::kOffline) {
    state_ = ConnectionState::kOffline;
    {
      std::lock_guard lock(mutex_);
      first_attempt_done_ = true;
    }
    REXKRNL_INFO("Live: offline (network_mode = offline)");
    return;
  }

  LiveApiConfig config = ConfigFromSettings(title_id);
  config.timeout_ms = 10000;
  config.connect_timeout_ms = 5000;
  api_ = std::make_shared<LiveApi>(config, KernelLogSink());
  state_ = ConnectionState::kConnecting;
  REXKRNL_INFO("Live: {} mode, server {}, gamertag \"{}\"", NetworkModeName(mode_),
               api_->api_address(), gamertag());

  thread_ = std::thread([this] { Run(); });
}

void LiveService::Stop() {
  if (!started_.load()) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (api_ && state_.load() == ConnectionState::kConnected) {
    // Xenia removes the console's sessions on exit, so nobody finds a dead match.
    LiveApiConfig config = api_->config();
    config.timeout_ms = 3000;
    config.connect_timeout_ms = 2000;
    LiveApi quick(config, KernelLogSink());
    quick.DeleteMySessions(identity().mac_address, Ipv4ToString(online_ip_.load()));
  }
}

void LiveService::WaitForFirstAttempt() {
  std::unique_lock lock(mutex_);
  if (first_attempt_done_) {
    return;
  }
  if (!wake_.wait_for(lock, kFirstAttemptWait, [this] { return first_attempt_done_; })) {
    if (!waited_too_long_) {
      REXKRNL_WARN("Live: the server is slow to answer; continuing as not signed in for now");
    }
    waited_too_long_ = true;
  }
}

bool LiveService::SignedInToLive() {
  if (!started_.load() || mode_ != NetworkMode::kOnline) {
    return false;
  }
  WaitForFirstAttempt();
  return state_.load() == ConnectionState::kConnected;
}

bool LiveService::Connected() {
  if (!started_.load() || mode_ == NetworkMode::kOffline) {
    return false;
  }
  WaitForFirstAttempt();
  return state_.load() == ConnectionState::kConnected;
}

bool LiveService::ConnectOnce() {
  const std::string host = HostFromServerAddress(api_->api_address());
  local_ip_ = DetectLocalIp(host);

  if (!api_->Connect(mode_)) {
    return false;
  }

  uint32_t ip = local_ip_.load();
  if (mode_ == NetworkMode::kOnline) {
    ip = ParseIpv4(api_->online_ip());
    if (!ip) {
      REXKRNL_ERROR("Live: the server sent an address that is not IPv4");
      return false;
    }
    // A server on this PC sees 127.0.0.1; others must get an address they can reach.
    if (IsLoopbackIp(ip) && local_ip_.load()) {
      REXKRNL_INFO("Live: server is on this PC; using local address {}", ShowIp(local_ip_.load()));
      ip = local_ip_.load();
    }
  }
  online_ip_ = ip;

  const Identity& id = identity();
  // Sessions left from an earlier run that ended badly.
  api_->DeleteMySessions(id.mac_address, Ipv4ToString(ip));

  PlayerInfo player;
  player.xuid = id.online_xuid;
  player.gamertag = gamertag();
  player.machine_id = MachineIdFromMac(id.mac_address);
  player.host_address = Ipv4ToString(ip);
  player.mac_address = id.mac_address;
  if (!api_->RegisterPlayer(player)) {
    REXKRNL_ERROR("Live: the server did not accept the player registration");
    return false;
  }

  if (auto ports = api_->GetPortMappings()) {
    if (!ports->connect.empty() || !ports->bind.empty()) {
      REXKRNL_INFO("Live: server lists {} connect / {} bind port mappings for this game",
                   ports->connect.size(), ports->bind.size());
    }
  }

  REXKRNL_INFO("Live: {} as {} (local address {})",
               mode_ == NetworkMode::kOnline ? "signed in to Live" : "LAN server reachable",
               ShowIp(ip), ShowIp(local_ip_.load()));
  return true;
}

void LiveService::Broadcast(bool connected) {
  auto* kernel = REX_KERNEL_STATE();
  if (!kernel) {
    return;
  }
  // XLiveAPI::BroadcastNetworkStatus, plus a sign-in change so the game re-reads
  // the profile's sign-in state.
  if (mode_ == NetworkMode::kOnline) {
    kernel->BroadcastNotification(
        kXNotificationLiveConnectionChanged,
        connected ? X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED : X_ONLINE_S_LOGON_DISCONNECTED);
    kernel->BroadcastNotification(kXNotificationLiveLinkStateChanged, 1);
    kernel->BroadcastNotification(kXNotificationSystemSignInChanged, 1);
  } else {
    kernel->BroadcastNotification(kXNotificationLiveConnectionChanged,
                                  X_ONLINE_S_LOGON_DISCONNECTED);
    kernel->BroadcastNotification(kXNotificationLiveLinkStateChanged, 1);
  }
}

void LiveService::Run() {
  bool ok = ConnectOnce();
  state_ = ok ? ConnectionState::kConnected : ConnectionState::kFailed;
  bool announce = false;
  {
    std::lock_guard lock(mutex_);
    first_attempt_done_ = true;
    announce = waited_too_long_ && ok;  // the game already went on without us
  }
  wake_.notify_all();
  if (announce) {
    Broadcast(true);
  }
  if (!ok) {
    REXKRNL_WARN("Live: will keep trying to reach the server every {} seconds",
                 std::chrono::duration_cast<std::chrono::seconds>(kRetryInterval).count());
  }

  for (;;) {
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, ok ? kHeartbeatInterval : kRetryInterval, [this] { return stopping_; });
      if (stopping_) {
        return;
      }
    }
    if (ok) {
      if (!api_->Heartbeat()) {
        ok = false;
        state_ = ConnectionState::kFailed;
        REXKRNL_WARN("Live: lost the connection to the server");
        Broadcast(false);
      }
    } else if (api_->Heartbeat() && ConnectOnce()) {
      ok = true;
      state_ = ConnectionState::kConnected;
      ForgetRemotes();
      Broadcast(true);
    }
  }
}

LiveService::RemotePlayer LiveService::LookupRemote(uint32_t ip) {
  {
    std::lock_guard lock(cache_mutex_);
    const auto it = remotes_.find(ip);
    if (it != remotes_.end()) {
      return it->second;
    }
  }
  RemotePlayer remote;
  const auto api = api_;
  if (api && state_.load() == ConnectionState::kConnected) {
    if (auto player = api->FindPlayer(Ipv4ToString(ip))) {
      remote.mac_address = player->mac_address;
      remote.session_id = player->session_id;
      std::lock_guard lock(cache_mutex_);
      remotes_[ip] = remote;
    }
  }
  return remote;
}

void LiveService::ForgetRemotes() {
  std::lock_guard lock(cache_mutex_);
  remotes_.clear();
}

void LiveService::PublishQos(uint64_t session_id, std::vector<uint8_t> data) {
  const auto api = api_;
  if (!api || state_.load() != ConnectionState::kConnected) {
    return;
  }
  {
    std::lock_guard lock(cache_mutex_);
    auto& published = qos_published_[session_id];
    if (published == data) {
      return;  // XLiveAPI::UpdateQoSCache: only send changes
    }
    published = data;
  }
  std::thread([api, session_id, data = std::move(data)] {
    api->PostQos(session_id, data);
  }).detach();
}

void LiveService::ForgetQos(uint64_t session_id) {
  std::lock_guard lock(cache_mutex_);
  qos_published_.erase(session_id);
}

}  // namespace rex::kernel::xam::live
