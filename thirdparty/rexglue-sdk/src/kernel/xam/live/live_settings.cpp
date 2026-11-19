/**
 * @file        kernel/xam/live/live_settings.cpp
 * @brief       Online play settings and the glue between LiveApi and the runtime
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */

#include <rex/kernel/xam/live/live_settings.h>

#include <cstring>

#include <rex/logging.h>

REXCVAR_DEFINE_STRING(network_mode, "offline", "Live",
                      "Online play: offline, online (Xbox Live replacement through api_address), "
                      "or lan (System Link games only). Xenia's 0, 1, 2 also work.")
    .allowed({"offline", "online", "lan", "0", "1", "2"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(api_address, "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/", "Live",
                      "Xenia Web Services server: the public Xenia Netplay server, or one on "
                      "your network such as 192.168.1.20:36000")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(upnp, false, "Live", "Open the game's ports on the router automatically (UPnP)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(upnp_root, "", "Live",
                      "The router's UPnP description URL (e.g. http://192.168.1.1:5000/rootDesc.xml). "
                      "Leave empty to find the router automatically.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(log_mask_ips, true, "Live",
                    "Hide the last part of IP addresses in the log");

REXCVAR_DEFINE_INT32(net_test_instance, 0, "Live",
                     "Testing two copies of the game on one PC: 1 or 2 gives this copy its own "
                     "private address (127.0.0.11 / 127.0.0.12). 0 = off (normal play)")
    .range(0, 9)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(gamertag, "Player", "Live",
                      "Your player name, shown to other players (at most 15 characters)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::kernel::xam::live {

uint32_t TestInstanceIp() {
  const int32_t n = REXCVAR_GET(net_test_instance);
  if (n <= 0 || n > 9) {
    return 0;
  }
  // 127.0.0.(10+n), network byte order like the other Live addresses.
  const uint8_t bytes[4] = {127, 0, 0, static_cast<uint8_t>(10 + n)};
  uint32_t ip;
  std::memcpy(&ip, bytes, 4);
  return ip;
}

NetworkMode CurrentNetworkMode() {
  return ParseNetworkMode(REXCVAR_GET(network_mode)).value_or(NetworkMode::kOffline);
}

LiveApiConfig ConfigFromSettings(uint32_t title_id) {
  LiveApiConfig config;
  config.api_address = REXCVAR_GET(api_address);
  config.title_id = title_id;
  config.mask_ips_in_log = REXCVAR_GET(log_mask_ips);
  return config;
}

LogSink KernelLogSink() {
  return [](LogLevel level, const std::string& text) {
    switch (level) {
      case LogLevel::kDebug:
        REXKRNL_DEBUG("{}", text);
        break;
      case LogLevel::kInfo:
        REXKRNL_INFO("{}", text);
        break;
      case LogLevel::kWarning:
        REXKRNL_WARN("{}", text);
        break;
      case LogLevel::kError:
        REXKRNL_ERROR("{}", text);
        break;
    }
  };
}

}  // namespace rex::kernel::xam::live
