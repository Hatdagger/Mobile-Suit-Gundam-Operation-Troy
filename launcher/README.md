# Operation Troy launcher

`Operation Troy.exe` sets up, builds and starts the PC recomp of
*Mobile Suit Gundam: Operation Troy*. It is made to be used with a controller
(keyboard and mouse work too). It contains **no game code and no game files**:
it works from your own copy of the game, and the PC version is built on your PC.

## What it does

1. **Finds your copy of the game**: a Games on Demand (GOD) folder or an `.iso`.
   It looks beside the launcher, in your user folders, and in folders such as
   `Games` or `Emulation` on each drive; you can also choose it yourself.
2. **Checks the build tools**: Visual Studio Build Tools with C++ and clang 18+,
   CMake 3.25+, Ninja. If they are missing it can start Microsoft's installer
   (you accept Microsoft's license there). Ninja is downloaded automatically if needed.
3. **Gets the recomp source** (only when it is not already in a git copy of
   this repository): with git, or from the latest release's `optroy-source.zip`.
4. **Extracts the game files** from your copy (about 1.4 GB).
5. **Builds the PC version**: converts the game's code and compiles it. The
   first build takes 30-90 minutes; later builds only recompile what changed.
6. **Play**, **Settings** (resolution, upscaling, anti-aliasing, V-Sync, your
   player name, ...) and **Update** (gets the newest version and rebuilds).

Both the original Japanese game and the fan English patch work, as a GOD folder
or an `.iso` (the patch only changes menu files, not the game's code).

## Player name

Your name in the game and in online matches. Set it in **Settings, PLAYER** (the
launcher also asks once, before the first game start). It is typed on an
on-screen keyboard with the controller, or straight on a keyboard:

| Action      | Controller | Keyboard            |
|-------------|------------|---------------------|
| Type a key  | A          | type normally       |
| Delete      | X          | Backspace           |
| Space       | Y          | Space               |
| Capitals    | LB / RB    | Shift               |
| Done (save) | Menu       | Enter               |
| Cancel      | B          | Esc                 |

Up to 15 letters, numbers, spaces and `- _ .`. It is saved as `gamertag` in
`gundam_operation_troy.toml`.

## Where things go

| Launcher is...                         | Source, game files, build      | Saves     | Log and settings       |
|----------------------------------------|--------------------------------|-----------|------------------------|
| inside a git copy of this repository   | the repository folder          | `saves\`  | `out\build.log`, `out\launcher.ini` |
| on its own in a folder                 | `RecompData\source\`           | `Saves\`  | `RecompData\build.log`, `RecompData\launcher.ini` |

The game's own settings file is `gundam_operation_troy.toml` beside the built game.

## Controls

| Action        | Controller   | Keyboard            |
|---------------|--------------|---------------------|
| Move          | D-pad / left stick | Arrows / WASD |
| OK            | A            | Enter / Space       |
| Back          | B            | Esc / Backspace     |
| Build log     | Y            | Y                   |
| Choose .iso   | X            | X                   |
| Tabs, pages   | LB / RB      | Q / E, Page Up/Down |
| Fullscreen    |              | F11                 |

## Building the launcher

GitHub Actions builds it on every change (`.github/workflows/launcher.yml`); the
`.exe` is in the run's Artifacts. To build it yourself on Windows:

```
git submodule update --init --depth 1 thirdparty/rexglue-sdk/thirdparty/sdl3 thirdparty/rexglue-sdk/thirdparty/imgui
cmake -S launcher -B out/launcher -A x64
cmake --build out/launcher --config Release
```

It uses SDL3 and Dear ImGui from `thirdparty/rexglue-sdk/thirdparty`, and the
Anton, Fira Sans and Anonymous Pro fonts (SIL Open Font License, see `fonts/`).

`--screenshots <folder>` renders every screen with example data into image files
(with `-DOPTROY_LAUNCHER_HEADLESS=ON` this works on machines without a display).
`-DOPTROY_LAUNCHER_TOOL=ON` adds `optroy_launcher_tool`, a command-line check of
the extractor, the game finder and the build steps.
