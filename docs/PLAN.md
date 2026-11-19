# Plan

## The game

- Mobile Suit Gundam: Operation Troy, Xbox 360, Japan only, released 26 June 2008
  (developed by Dimps / Dream Execution; the Western release "Mobile Ops: The One Year War" was cancelled).
- Title ID `424107D6`.
- Code modules: `default.xex` (10.9 MB) and `Contents/dlls/PathEngine_Xenon_Release.xex`
  (PathEngine pathfinding middleware, 1.0 MB).
- Online play was **Xbox Live only** (up to 10 players, Federation vs Zeon).
  The original game has **no System Link/LAN and no split-screen**.
- Confirmed: the game's Live mode works in Xenia Canary Netplay. So its online code
  runs correctly against that Xbox Live replacement.

## Rules for this repository

- The repository is public and holds **no game code and no game assets**: no XEX files,
  no converted (recompiled) code, no textures, sounds or levels.
- Everyone builds from their own copy of the game. The build converts the game's code
  locally, on the player's own PC.

## Steps

1. **Bring-up:** convert the game's code with ReXGlue, build, and get the game booting
   to its menus, then playable in Mission Mode (single player).
2. **Xbox Live replacement:** port the netplay layer from Xenia Canary Netplay into the
   recomp's Xbox 360 system layer (ReXGlue's `kernel/xam`): network addresses, Live
   sign-in, and creating, searching, joining, starting and ending matches. Matches are
   brokered by a Xenia Web Services server.
3. **Online:** use the public Xenia Netplay server (with credit), with the server
   address as a setting so anyone can switch to their own server.
4. **LAN:** the host runs the same server locally (ideally built in), and players on
   the same network find each other's matches.
5. **Front end:** a full, controller-driven launcher (`launcher/`): finds the player's
   copy, checks and installs build tools, extracts the game files, builds, plays, and
   holds the settings. Built by GitHub Actions; contains no game code or assets.

## Online play progress

Six parts:

1. **Foundation — done.** An HTTP/HTTPS client (WinHTTP on Windows, the system libcurl
   on Linux), a client for Xenia Web Services servers ported from Xenia Canary Netplay
   (`thirdparty/rexglue-sdk/src/kernel/xam/live/`), and the settings `network_mode`,
   `api_address`, `upnp`, `log_mask_ips`. Tested without the game by
   [`tools/netcheck`](../tools/netcheck/README.md) against a mock server; GitHub Actions
   ("Netplay") runs that test on Windows and Linux. The game does not use it yet.
2. **Network addresses and Live sign-in — done.** Each PC gets its own network identity
   (`live_identity.toml` in the saves folder), connects to the server when the game starts,
   signs the profile in to Live with the `gamertag` setting, and reports real network
   addresses and QoS data to the game. Only active with `network_mode = "online"` (or
   `"lan"`); the default stays offline.
3. Creating, finding and joining matches (needs the game running to its menus).
4. Port mapping and UPnP.
5. A small built-in server for LAN play, started from the launcher.
6. Tests between two PCs, and with Xenia Netplay players.

## Status of the Xbox Live layer in ReXGlue v0.10.0

- Ordinary network traffic already works (sockets are mapped to the PC's network).
- Xbox Live functions are placeholders: network addresses report loopback, address
  translation fails, match functions log and return success without doing anything, and
  Live sign-in reports offline. Files: `src/kernel/xam/xam_net.cpp`,
  `src/kernel/xam/apps/xgi_app.cpp`, `src/kernel/xam/apps/xlivebase_app.cpp`.
