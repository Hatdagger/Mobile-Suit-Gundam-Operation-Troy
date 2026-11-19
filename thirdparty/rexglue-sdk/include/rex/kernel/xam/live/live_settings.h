/**
 * @file        kernel/xam/live/live_settings.h
 * @brief       Online play settings (the "Live" section of the game's .toml)
 *
 * The names match Xenia Canary Netplay's settings where they overlap:
 *
 *   network_mode = "offline"   # "offline", "online", or "lan" (System Link only)
 *   api_address  = "https://xenia-netplay-2a0298c0e3f4.herokuapp.com/"
 *                              # or a server on your network, e.g. "192.168.1.20:36000"
 *   upnp         = false       # open ports on the router automatically
 *   log_mask_ips = true        # hide the last part of IP addresses in the log
 *   gamertag     = "Player"    # your name for other players (15 characters at most)
 *
 * @copyright   Copyright (c) 2026 Gundam Operation Troy recomp contributors
 * @license     BSD 3-Clause License
 */
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/kernel/xam/live/live_api.h>

REXCVAR_DECLARE(std::string, network_mode);
REXCVAR_DECLARE(std::string, api_address);
REXCVAR_DECLARE(bool, upnp);
REXCVAR_DECLARE(bool, log_mask_ips);
REXCVAR_DECLARE(std::string, gamertag);

namespace rex::kernel::xam::live {

// The network_mode setting (offline if it holds something unknown).
NetworkMode CurrentNetworkMode();

// A LiveApi configuration from the settings above.
LiveApiConfig ConfigFromSettings(uint32_t title_id);

// Sends LiveApi messages to the kernel log.
LogSink KernelLogSink();

}  // namespace rex::kernel::xam::live
