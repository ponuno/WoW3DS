# WoW3DS

A World of Warcraft **1.12.1 (Vanilla)** client for the **New Nintendo 3DS / New 2DS XL**, written from scratch in C++ for
devkitARM (libctru + citro3d), plus a PC converter that turns *your own* 1.12 game files into packs the console can stream.

> **Status: experimental hobby project.** Most features are tested on a PC against stand-in code (a stubbed libctru and an
> independent Python server). Only the items listed under "Verified on hardware" were run on a real console and a real
> server. Anything else should be treated as unverified. [`PLAN.md`](PLAN.md) is the detailed, honest per-stage status.

No game data, textures, models or sounds are in this repository. You need your own legally obtained WoW 1.12.1 client
(`Data/*.MPQ`) and a private server that speaks the 1.12 protocol (the client was developed against a vMaNGOS-compatible
server).

## What works

- **World:** terrain, water, doodads and WMO buildings streamed per tile; walking with floors, stairs and walls
  (collision from WMO and M2 data); continents and instance maps as separate packs (`map<ID>.w3dp`); dungeons are
  WDT-only maps converted from their global WMO; in-game **loading screens** and a loading gate that keeps the character
  frozen until the ground and its collision are ready.
- **Characters and creatures:** skinned, animated creature and player models (race, skin, face, hair, armour layers,
  helmets, weapons), ships and zeppelins, ordinary game objects (event decorations, chests, ...).
- **Gameplay client:** login, realm and character screens, chat, targeting, auto-attack, spells and action bar, loot,
  vendors, trainers, quests, items and tooltips, auras, parties, reputation, talents, flight masters, mail/auction/trade
  windows (see `PLAN.md` for the exact list).
- **Controls:** a controller scheme built for the 3DS ("Forever" layout, or "Classic"), touch-screen UI on the bottom
  screen, top screen modes: normal 400x240, wide 800x240, or stereoscopic 3D.
- **Sound:** interface sounds, footsteps, world sounds, zone music and ambience (from your client's files, DSP firmware
  dump required on the console).

## Verified on hardware

Run on a New 3DS against a private 1.12 server: rendering of one tile (60 FPS), world streaming (59 FPS), login and
entering the world, movement and world objects. Everything from stage 5.0 onward that is not in that list (buildings,
creature models, UI windows, dungeons, loading screens, map switching, ...) is tested on the PC and still needs a
console run. The most recent additions (dungeons, loading screens, the loading gate) have not been run on a console at
all.

## Repository layout

| Path | What |
|---|---|
| `proto3ds/` | The 3DS application: streamer, renderer, controls, game UI, audio. `source/`, PC tests in `tests/` |
| `net/` | Game protocol and world state (auth, world session, objects, movement, spells, quests, ...). Plain C++20, also built on the PC for tests (`net/tools/w3dlogin.cpp`) |
| `converter/` | `w3dconv` (command line), `w3dgui` (Windows GUI), `w3dpreview` (PNG previews) and their tests |
| `common/` | Shared binary formats (`w3d_format.h`, `w3d_ui.h`, ...) used by converter and client |
| `tools/` | `armcheck` (ARM compile check on a PC), font generator |
| `docs/DEVLOG_RU.md` | The original development log (Russian) |
| `PLAN.md` | Staged plan and per-stage status |

## Building

### Converter (PC, Linux or Windows via MinGW)

Needs CMake and a C++20 compiler. Dependencies are fetched by CMake: glm, meshoptimizer, rg-etc1, WoWee (format parsers
only) and optionally StormLib (`-DW3D_STORMLIB=ON`, default) to read the MPQ archives directly.

```
cd converter
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 3DS application

Needs devkitPro with devkitARM, libctru and citro3d (`DEVKITARM` set).

```
cd proto3ds
make            # produces w3dproto.3dsx
```

## Converting your game files

Point the converter at the client's `Data` folder (the MPQ archives are read directly). The Windows GUI (`w3dgui.exe`)
runs all of the steps below with checkboxes; on the command line:

```
# interface, texts and in-game loading screens
w3dconv --data <WoW>/Data --ui -o out/ui.w3dp
# sound and music
w3dconv --data <WoW>/Data --sound -o out/sound
# Eastern Kingdoms (map 0 -> test.w3dp) or Kalimdor (map 1 -> map1.w3dp); a tile region or --world for all tiles
w3dconv --data <WoW>/Data --map Azeroth --tiles 30 46 34 50 --creatures --players --transports -o out/test.w3dp
# every instance map -> out/map<ID>.w3dp
w3dconv --data <WoW>/Data --dungeons --creatures --players -o out/
```

Run `w3dconv` without arguments for all options. Texture quality (`--tex-quality`), triangle budgets, texture sizes and
collision options are adjustable. Each map pack carries its own creature and player models, so full runs are large and
slow.

Copy the output to the SD card as `sdmc:/3ds/w3dproto/` (`test.w3dp`, `map<ID>.w3dp`, `ui.w3dp`, the `sound/` folder and
the text files the converter writes next to the packs), then start `w3dproto.3dsx` through the Homebrew Launcher.

## Tests (no console and no game data needed)

```
converter/tests/run_wmo_tests.sh <build dir>        buildings, collision, previews
converter/tests/run_dungeon_tests.sh <build dir>    instance maps, loading screens
converter/tests/run_player_tests.sh <build dir>     player models
converter/tests/run_transport_tests.sh <build dir>  ships and zeppelins
converter/tests/run_gui_tests.sh <build dir>        GUI settings logic
proto3ds/tests/run_streamer.sh <pack.w3dp>          the streamer on the PC with sanitizers
proto3ds/tests/run_ui.sh, run_hud.sh, run_anim.sh   UI, HUD and animation checks
net/tests/run_stage5.sh                             protocol tests against an independent Python server
tools/armcheck/armcheck.sh <files>                  ARM compile check (needs a devkitPro-like sysroot)
```

Some net and UI tests replay a recorded real-server session that is not part of this repository.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Blizzard Entertainment. "World of Warcraft" is a
trademark of Blizzard Entertainment. Do not share converted packs: they contain data derived from the game. The
converter uses [WoWee](https://github.com/Kelsidavis/WoWee) (MIT) for file-format parsing.
