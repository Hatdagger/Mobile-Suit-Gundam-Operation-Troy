# Third-party code

## rexglue-sdk

A copy of the ReXGlue SDK v0.10.0, taken from
[Sirhalo23/rexglue-sdk](https://github.com/Sirhalo23/rexglue-sdk) at commit `c94f5eb`
(a snapshot of [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk) v0.10.0).
It is kept in this repository so this project can change it, for example to add the
Xbox Live replacement needed for online play. See `rexglue-sdk/LICENSE`.

Changes from the original:
- `tools/binutils/` was left out (PowerPC tools used only by the SDK's own tests).
- The SDK's helper libraries (`rexglue-sdk/thirdparty/*`) are git submodules of this
  repository, pinned to the same commits the SDK uses.

- `src/system/CMakeLists.txt`: the runtime now exports imgui's header folder, so a
  game project can build against the SDK source tree (public SDK headers include
  `<imgui.h>`; the installed SDK package ships it, the source tree did not).

- **Online play, part 1** (the Xbox Live replacement's foundation; nothing calls it yet):
  - `include/rex/net/http.h`, `src/core/http_win.cpp`, `src/core/http_curl.cpp`: a small
    HTTP/HTTPS client. Windows uses WinHTTP; Linux and macOS load the system libcurl at
    run time (no build dependency).
  - `include/rex/kernel/xam/live/live_api.h`, `src/kernel/xam/live/live_api.cpp`: a client
    for Xenia Web Services servers, ported from Xenia Canary Netplay's `XLiveAPI`
    ([AdrianCassar/xenia-canary](https://github.com/AdrianCassar/xenia-canary), branch
    `netplay_canary_experimental`, commit `6342e12`, BSD licence): who-am-I, players,
    sessions (create, search, details, join, leave, modify, migrate, arbitration, delete)
    and QoS, with the same requests as Xenia Netplay.
  - `include/rex/kernel/xam/live/live_settings.h`, `src/kernel/xam/live/live_settings.cpp`:
    the settings `network_mode`, `api_address`, `upnp` and `log_mask_ips`.
  - `thirdparty/rapidjson`: new submodule ([Tencent/rapidjson](https://github.com/Tencent/rapidjson),
    MIT licence, header-only), used only inside `live_api.cpp`.
  - Build files: `src/core/CMakeLists.txt` (WinHTTP / dl), `src/kernel/CMakeLists.txt`,
    `thirdparty/CMakeLists.txt` (rapidjson target), `cmake/rexglue_install.cmake`.

- **Online play, part 2** (network addresses and Live sign-in; only active when
  `network_mode` is `online` or `lan`, so offline play behaves as before):
  - `include/rex/kernel/xam/live/live_identity.h`, `src/kernel/xam/live/live_identity.cpp`:
    this PC's network identity (an Xbox 360 MAC address and an online XUID, made once
    and kept in `live_identity.toml` in the user data folder), the `gamertag` setting's
    rules, and address helpers.
  - `include/rex/kernel/xam/live/live_service.h`, `src/kernel/xam/live/live_service.cpp`:
    started with the game (`src/system/runtime.cpp`); connects to the server in the
    background, registers the player, retries while the server is down, tells the game
    when the connection comes or goes, and removes this PC's sessions on exit.
  - `include/rex/kernel/xam/live/xnet.h`: XNADDR, XNKID, QoS and other network structures
    (from Xenia Canary Netplay's `xnet.h`).
  - `src/kernel/xam/xam_net.cpp`: real XNet addresses (`XNetGetTitleXnAddr`,
    `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetXnAddrToMachineId`,
    `XNetInAddrToString`), QoS (`XNetQosListen` publishes to the server,
    `XNetQosLookup` reads from it, `XNetQosServiceLookup`), DNS lookups, keys,
    `XNetRandom`, link status and a few more former stubs, after Xenia Canary Netplay's
    `xam_net.cc`.
  - `src/system/xam/user_profile.cpp`, `include/rex/system/xam/user_profile.h`,
    `src/kernel/xam/xam_user.cpp`: the profile signs in to Live (state 2) while online,
    with the online XUID, the gamertag as its name, and the Live privileges. Save data
    keeps its folder name (`src/system/xam/content_manager.cpp`).
  - `src/kernel/xboxkrnl/xboxkrnl_xconfig.cpp`: reports the same MAC address.

Any further changes made by this project are listed in the commit history.
