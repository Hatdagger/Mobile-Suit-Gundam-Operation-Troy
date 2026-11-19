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

## Getting the files the build needs

The build only needs the game's program files. A helper script pulls them out of your copy:

```
python tools/extract_game.py "<path to your GOD folder or .iso>"
```

This writes `game/default.xex` and `game/Contents/dlls/PathEngine_Xenon_Release.xex`.
Add `--all` to extract every game file (needed to actually run the game).
The `game/` folder is ignored by git, so it can never be committed by accident.

Building instructions will be added once the first build works.

## How online play will work

The original game only supported Xbox Live, which no longer exists for the Xbox 360.
This project replaces the Xbox Live parts the game uses (signing in, creating, finding and
joining matches) with an open replacement, based on the work of
[Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary) and
[Xenia Web Services](https://github.com/AdrianCassar/Xenia-WebServices).

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom (crack) and contributors — the recompilation toolkit (vendored in `thirdparty/rexglue-sdk`, from [Sirhalo23/rexglue-sdk](https://github.com/Sirhalo23/rexglue-sdk), v0.10.0).
- [Xenia](https://github.com/xenia-project/xenia) — the Xbox 360 emulator ReXGlue is built on.
- [Xenia Canary Netplay](https://github.com/AdrianCassar/xenia-canary) and [Xenia Web Services](https://github.com/AdrianCassar/Xenia-WebServices) by Adrian Cassar and contributors — the Xbox Live replacement this project's online play builds on.

This project is not affiliated with or endorsed by Bandai Namco, Sunrise, Sotsu, Microsoft or Xbox.
All trademarks belong to their respective owners.
