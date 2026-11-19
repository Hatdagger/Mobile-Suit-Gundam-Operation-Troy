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

Any further changes made by this project are listed in the commit history.
