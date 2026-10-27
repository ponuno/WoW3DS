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
- **Water:** lakes, rivers, the sea, lava and slime of the terrain and of buildings (Stormwind's canals, dungeon pools)
  with the game's own animated textures and Light.dbc colours; swimming (diving with the camera, swimming up, jumping
  out), falling into water, water walking, the underwater view, breath / fatigue bars, drowning and lava damage, fishing.
- **Characters and creatures:** skinned, animated creature and player models (race, skin, face, hair, armour layers,
  helmets, weapons), ships and zeppelins, ordinary game objects (event decorations, chests, ...).
- **Gameplay client:** login, realm and character screens, chat, targeting, auto-attack, spells and action bar, loot,
  vendors, trainers, quests, items and tooltips, auras, parties, reputation, talents, flight masters, mail/auction/trade
  windows (see `PLAN.md` for the exact list).
- **Controls:** a controller scheme built for the 3DS ("Forever" layout, or "Classic"), touch-screen UI on the bottom
  screen, top screen modes: normal 400x240, wide 800x240, or stereoscopic 3D.
- **Sound:** interface sounds, footsteps, world sounds, zone music and ambience (from your client's files, DSP firmware
  dump required on the console).



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

## Using the Windows GUI converter (`w3dgui.exe`)

`w3dgui.exe` runs the whole conversion in one window. Keep it in the same folder as `w3dconv.exe` (and `w3dpreview.exe`);
it starts `w3dconv.exe` once per step and shows its output. Use paths without non-Latin characters.

1. **Data folder:** the `Data` folder of your WoW 1.12 client (the one with the `.MPQ` files). The archives are read
   directly, nothing has to be extracted.
2. **Output folder:** one folder for everything. Copy its contents to `sdmc:/3ds/w3dproto/` on the SD card, or pick
   `<SD>:\3ds\w3dproto` here directly.
3. **World group**
   - *Build the world pack*: the terrain pack of one map. **Map:** `Azeroth` (Eastern Kingdoms) gives `test.w3dp`,
     `Kalimdor` gives `map1.w3dp`; another map directory name gives `<name>.w3dp`, which you must rename to `map<ID>.w3dp`
     yourself (use *Dungeons* below for instances).
   - *Whole world* converts all tiles (hours of work and a lot of disk space; a pack cannot exceed 4 GB, the FAT32 limit).
     *Tile region* converts only the tiles between "x from / y from" and "x to / y to" (0..63), the quick way to test
     (default 30,46 - 34,50 is Elwynn Forest and Northshire).
   - *Textures*: `fast`, `better`, `best` terrain/model texture compression. `fast` is much quicker, slightly blurrier.
   - *Creatures*, *Players*, *Armor, weapons*, *Ships, zeppelins*, *All game objects (big)*: which models go into the pack.
     Without *Players* other characters are not drawn, and without *Creatures* there are no monsters or NPCs.
     *All game objects* adds every ordinary object model (event decorations, chests, ...); narrow it with *Extra*
     (e.g. `--gameobjects pumpkin`).
4. **Interface and texts:** makes `ui.w3dp` (icons, window skin, fonts, zone maps, in-game loading screens) and the text
   files (`spells.txt`, `factions.txt`, `talents.txt`, `taxi.txt`, ...). Without `ui.w3dp` the 3DS draws a plain stub skin.
5. **Sound and music:** makes the `sound` folder.
6. **Dungeons and raids:** one `map<ID>.w3dp` per instance map, with the same Creatures/Players/Armor options (every pack
   carries its own model copies, so this is slow and big; leave it off until the open world works).
7. **EVERYTHING:** one run of `w3dconv --all` instead of the steps above: interface, sound, every map and dungeon of
   Map.dbc with creatures, players and ships, all sharing **one asset pool** (see below). The world and dungeon choices
   are greyed out; *Textures*, *Armor, weapons*, *All game objects*, *Interface*, *Sound* and *Extra* still apply.
8. **Extra:** any more `w3dconv` options for the world pack, typed as on the command line.
9. Press **Start**. The steps run one after another (interface, sound, world, dungeons); the progress bar and log show
   the current step, **Cancel** stops it. When it says "Done", press **Open the output folder** and copy everything
   to the SD card.

Each step also writes a log into the output folder (`log_ui.txt`, `log_sound.txt`, `log_world.txt`, `log_dungeons.txt`);
send those when something looks wrong. The GUI remembers its settings in `w3dgui.ini` next to the exe. The password
of the game server is never asked for here.

## Converting your game files from the command line

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
# EVERYTHING in one run with a shared asset pool (all of the above, every map)
w3dconv --data <WoW>/Data --all -o out/
```

Run `w3dconv` without arguments for all options. Texture quality (`--tex-quality`), triangle budgets, texture sizes and
collision options are adjustable. Packs made one by one each carry their own copy of every model and texture they use.

**Shared pool (`--all`).** `map<ID>.w3dp` then holds only the tiles of a map (terrain, water, floors and walls,
placements); every texture, model, building, creature, player and item model is stored once in `common.w3dp` (split
into `common1.w3dp`, `common2.w3dp`, ... below 3.9 GB for FAT32; `--pool-part-mb`). The 3DS keeps every part open and
reads an asset straight from its file, so streaming costs the same as with a standalone pack. All files of one run
carry the same build id: a map pack is refused with a clear message next to a pool of another run. `map0.w3dp` is the
Eastern Kingdoms (preferred over `test.w3dp`), `map1.w3dp` Kalimdor. Developer test maps are skipped; `--maps 0,1,36`
picks maps, `--no-ui` / `--no-sound-step` skip those steps. `w3dconv --info` and `w3dpreview` resolve the pool.

Copy the output to the SD card as `sdmc:/3ds/w3dproto/` (`test.w3dp` or `map0.w3dp`, `map<ID>.w3dp`, `common*.w3dp`,
`ui.w3dp`, the `sound/` folder and the text files the converter writes next to the packs), then start `w3dproto.3dsx`
through the Homebrew Launcher.

## Tests (no console and no game data needed)

Shared pool and `--all`: `converter/tests/run_pool_tests.sh <build dir>`.
Liquids: `converter/tests/run_water_tests.sh <build dir>`, `proto3ds/tests/run_swim.sh`, and `water_test` inside
`net/tests/run_stage5.sh`.

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
