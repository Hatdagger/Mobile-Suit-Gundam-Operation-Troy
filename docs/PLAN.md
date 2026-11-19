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
5. **Front end:** to be decided (in-game settings vs. a separate launcher).

## Status of the Xbox Live layer in ReXGlue v0.10.0

- Ordinary network traffic already works (sockets are mapped to the PC's network).
- Xbox Live functions are placeholders: network addresses report loopback, address
  translation fails, match functions log and return success without doing anything, and
  Live sign-in reports offline. Files: `src/kernel/xam/xam_net.cpp`,
  `src/kernel/xam/apps/xgi_app.cpp`, `src/kernel/xam/apps/xlivebase_app.cpp`.
