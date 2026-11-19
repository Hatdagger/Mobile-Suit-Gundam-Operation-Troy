# Mobile Suit Gundam: Operation Troy — PC Recomp

A native PC port of **Mobile Suit Gundam: Operation Troy** (Xbox 360, 2008), made by
statically recompiling the original game with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**Goal:** the full game running natively on PC, with its **online multiplayer working again**,
over the internet and on a local network (LAN).

> **Status: early development.** Nothing is playable yet. See [docs/PLAN.md](docs/PLAN.md).

## You need your own copy of the game

This repository contains **no game code and no game assets**, and never will.
To build and play, you need your own copy of *Gundam Operation Troy* (Xbox 360),
as a disc image (`.iso`) or a Games on Demand (GOD) folder dumped from your own disc.

Do not open issues or pull requests that contain game files, and do not ask for them.

## The launcher

**`Operation Troy.exe`** sets everything up and is used with a controller (keyboard and
mouse work too): it finds your copy of the game, checks the build tools, extracts the game
files, builds the PC version on your PC, and starts it. It contains no game files.
Get it from the latest [Launcher run](../../actions/workflows/launcher.yml) (Artifacts), or
from Releases once one is published. See [launcher/README.md](launcher/README.md).

Put it in your copy of this repository (or in an empty folder) and run it.

## Getting the files the build needs (without the launcher)

The build only needs the game's program files. A helper script pulls them out of your copy:

```
python tools/extract_game.py "<path to your GOD folder or .iso>"
```

This writes `game/default.xex` and `game/Contents/dlls/PathEngine_Xenon_Release.xex`.
Add `--all` to extract every game file (needed to actually run the game).
The `game/` folder is ignored by git, so it can never be committed by accident.

Or double-click `build.bat`, which does the same as the launcher's setup in a console window
(it needs Python for the extraction step). A log is written to `out\build.log`.

## How online play will work

The original game only supported Xbox Live, which no longer exists for the Xbox 360.
This project replaces the Xbox Live parts the game uses (signing in, creating, finding and
joining matches) with an open replacement, based on the work of
[Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary) and
[Xenia Web Services](https://github.com/AdrianCassar/Xenia-WebServices).

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom (crack) and contributors — the recompilation toolkit (vendored in `thirdparty/rexglue-sdk`, from [Sirhalo23/rexglue-sdk](https://github.com/Sirhalo23/rexglue-sdk), v0.10.0).
- [Xenia](https://github.com/xenia-project/xenia) — the Xbox 360 emulator ReXGlue is built on.
- [Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary) and [Xenia Web Services](https://github.com/AdrianCassar/Xenia-WebServices) by Adrian Cassar and contributors — the Xbox Live replacement this project's online play builds on (its server client is ported from Xenia Canary Netplay's `XLiveAPI`, BSD licence).
- [RapidJSON](https://github.com/Tencent/rapidjson) by Tencent and Milo Yip (MIT licence) — reads and writes the server's JSON.

This project is not affiliated with or endorsed by Bandai Namco, Sunrise, Sotsu, Microsoft or Xbox.
All trademarks belong to their respective owners.
