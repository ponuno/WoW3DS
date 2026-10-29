# WoW3DS

A World of Warcraft **1.12.1 (Vanilla)** client for the **New Nintendo 3DS / New 2DS XL**, written from scratch in C++ for
devkitARM (libctru + citro3d), plus a PC converter that turns *your own* 1.12 game files into packs the console can stream.

> **Status: experimental hobby project.** Features are developed against PC tests (a stubbed libctru and an independent
> Python server) and then tried on a real New 3DS against a real server; [`PLAN.md`](PLAN.md) is the detailed, honest
> per-stage status, including what has not been confirmed on hardware yet.

No game data, textures, models or sounds are in this repository. You need your own legally obtained WoW 1.12.1 client
(`Data/*.MPQ`) and a private server that speaks the 1.12 protocol (the client was developed against a vMaNGOS-compatible
server).

## What works

- **World:** terrain, water, doodads and WMO buildings streamed per tile; continents and instance maps as separate packs
  (`map<ID>.w3dp`); dungeons, raids and the Deeprun Tram; in-game **loading screens** and a loading gate that keeps the
  character frozen until the ground and its collision are ready.
- **Walking (stage 6.18):** the exact ground of the terrain (`GRND`), unsimplified building and doodad collision, the
  original walking rules (step height, slopes steeper than 50 degrees cannot be climbed), **fall damage** (the server is
  told about long falls the way the original client does it).
- **Water:** lakes, rivers, the sea, lava and slime of the terrain and of buildings with the game's animated textures;
  swimming, diving, water walking, the underwater view, breath / fatigue bars, drowning and lava damage, fishing.
- **World objects:** elevators and trams, doors and gates (closed doors block), ships and zeppelins, tilted objects,
  player corpses, rings for ground spells, ordinary game objects (chests, herbs, ore, event decorations).
- **Characters and creatures:** skinned, animated creature and player models (race, skin, face, hair, armour layers,
  helmets, weapons).
- **Game client:** the original login, realm, character selection and creation screens (with their 3D scenes), chat,
  targeting, auto-attack, spells and hotbars, pets, loot, vendors, trainers, quests, items and tooltips, auras, parties,
  reputation, talents, **skills and their progress (weapon skills, professions)**, **descriptions of spells, talents and
  item effects** taken from your client's `Spell.dbc` (stage 6.20), flight masters, mail, auction house, bank, trade,
  duels. See [`PLAN.md`](PLAN.md) for the exact list.
- **Interface language:** **English by default everywhere** (also in the main menu); Russian can be switched on in the
  Settings tab.
- **Controls:** a controller scheme built for the 3DS ("Forever" layout, or "Classic" for the Old 3DS), touch-screen UI
  on the bottom screen; an optional ConsolePort skin (round crossbar buttons, the segmented experience bar) from your own
  copy of the addon. Top screen: normal 400x240, wide 800x240 or stereoscopic 3D.
- **Sound:** interface sounds, footsteps, world sounds, voices, zone music and ambience (DSP firmware dump required).

Not done yet: Warden (servers that require it will drop the connection), see `PLAN.md`.

## Installation (the release `.3dsx` + the converter pack)

### What you need

- A **New Nintendo 3DS / New 2DS XL** (an Old 3DS works with the Classic scheme, but slowly) with custom firmware
  (Luma3DS) and the **Homebrew Launcher**.
- `sdmc:/3ds/dspfirm.cdc` for sound: run the **DSP1** homebrew once if you do not have it.
- An SD card (FAT32) with room for the packs: a few hundred MB for a test region, several GB for the whole game.
- Your own **WoW 1.12.1 client** (the `Data` folder with the `.MPQ` files) and a Windows PC.
- **7-Zip** (https://www.7-zip.org) to unpack the converter pack.
- An account on a 1.12.1 private server (developed against vMaNGOS-compatible servers).

### Step 1. The 3DS application

Download `w3dproto.3dsx` from the releases (or build it, see *Building*) and copy it to the SD card as
`sdmc:/3ds/w3dproto/w3dproto.3dsx`. The same folder `sdmc:/3ds/w3dproto/` holds the packs and settings.

### Step 2. Unpack the converter pack

Unpack the converter pack (`w3dconv-win64.7z`) with 7-Zip into a folder **without non-Latin characters in its path**
(for example `C:\w3dconv`). It contains:

| File | What |
|---|---|
| `w3dgui.exe` | the converter with a window (recommended) |
| `w3dconv.exe` | the command-line converter (`w3dgui.exe` runs it) |
| `w3dpreview.exe` | PNG previews of a pack (maps, models, collision) |
| `all.bat` | **everything in one run**: interface, texts, sound, every map and dungeon |
| `ui.bat`, `sound.bat` | only the interface and texts / only the sound |
| `convert.bat` | a small test region (Elwynn Forest and Northshire) |
| `ui_skin.txt` | the interface skin description (keep it next to `w3dconv.exe`) |
| `README_RU.txt` | notes per converter version (Russian) |
| `w3dlogin.exe`, `*_test.bat` | developer tools (login and movement tests on the PC) |

**Optional, ConsolePort look:** put your own copy of the ConsolePort addon (the `ConsolePort` folder with
`ConsolePort` and `ConsolePort_Bar` inside, as in `Interface/AddOns`; the unpacked GitHub download `ConsolePort-master` works too) next to `w3dconv.exe`
before converting the interface; the log says `consoleport: ...`. Without it the plain crossbar is used.

### Step 3. Convert your game files

**With the window:** start `w3dgui.exe`, choose the client's `Data` folder and an output folder, tick **EVERYTHING** and
press **Start** (details below in *Using the Windows GUI converter*).

**With the batch file:** open a command prompt in the converter folder and run

```
all.bat "D:\WoW 1.12\Data" "D:\w3d_out"
```

The first argument is the client's `Data` folder, the second the output folder (it may be `<SD>:\3ds\w3dproto` on the
card itself). Without arguments `all.bat` uses the paths written at its top - edit them once with Notepad if you like.
The full conversion takes **hours** and several GB; `convert.bat` makes a quick test region first.

### Step 4. Copy to the SD card

Copy **everything in the output folder** except `log_*.txt` and `preview\` to `sdmc:/3ds/w3dproto/`:

```
sdmc:/3ds/w3dproto/
  w3dproto.3dsx
  map0.w3dp, map1.w3dp, map<ID>.w3dp ...   maps (Eastern Kingdoms, Kalimdor, dungeons)
  common.w3dp, common1.w3dp ...            the shared textures and models (they belong to the maps of the same run)
  ui.w3dp                                  interface, icons, fonts, zone maps, loading screens
  sound/                                   sound and music
  *.txt, spelldesc.bin                     texts: spells, spell descriptions, talents, skills, factions, flight paths,
                                           area triggers (dungeon doors, the tram), glue texts, transports ...
```

Packs of two different runs do not mix: after a new `all.bat` replace all `map*.w3dp` and `common*.w3dp` together.

### Step 5. Play

Start **w3dproto** from the Homebrew Launcher. On the login screen enter the **server** (the realmlist address, e.g.
`logon.example.com`), the **account** and the **password**, then choose the realm and your character. The server, the
account, the last realm and character are remembered in `login.cfg`; **the password is never saved**.

### Updating

Replace `w3dproto.3dsx`. When the release notes ask for it, also rerun the converter of the same release: `ui.bat` for
new texts or interface parts (for example `spelldesc.bin` and `skills.txt` of stage 6.19-6.20), `all.bat` for new
world data (collision, ground).

## Controls

### Forever scheme (default, New 3DS)

| Input | Action |
|---|---|
| Circle Pad | move (push fully to run, a little to walk); the character turns to the stick direction |
| C-Stick | camera; while L or R is held it aims the target ring |
| **B** | jump (held: swim up) |
| **A** | cancel (spell cast, target, open window) |
| **Y** | interact (talk, loot, open); in combat: attack |
| **X** | context menu of the target |
| D-pad | hotbar slots 1-4 |
| **ZL** / **ZR** / **ZL+ZR** (held) | hotbar sets 1 / 2 / 3 on the D-pad and the face buttons (28 slots per page) |
| **R** held (enemy mode) | X or D-pad: next enemy; Y attack; B last hostile target; A assist |
| **L** held (friendly mode) | X next friendly unit; D-pad party members 1-4; B target yourself; A assist |
| **L+R** held | D-pad left / right: hotbar page; up / down: camera zoom; A: centre the camera |
| START | menu |
| SELECT | free camera (tap); SELECT + D-pad left / right: switch the touch-screen tabs |

Ground spells (Blizzard, Flamestrike): a ring appears 10 yards ahead, the C-Stick moves it, press the spell button again
to cast, A cancels (or cast straight at the target: Settings, *Ground spells*). ZL / ZR can be switched to *press to
toggle* in the Settings.

### Classic scheme (Old 3DS)

Circle Pad up / down walk, left / right turn; C-Stick or D-pad left / right look, D-pad up / down zoom; B jump, L next
target, R attack, Y chat, A interact, X next hotbar set (on the touch bar), START menu.

### Touch screen

Tabs: **Chat, Bags, Gear, Spells** (spell book, talents, skills), **Quests, Combat, Map, Social, Settings**. Tap an item
or spell for its tooltip / description; in the Spells tab tap a spell, then press a button (with ZL / ZR for the other
sets) or tap a bar cell to put it on the hotbar. The Settings tab has the control scheme, the **language (English /
Russian)**, draw distance, number of creatures drawn, the top screen mode (wide / 3D), effects, volumes, FPS display and
the performance log.

### Login and character screens

D-pad and A to choose on the screen (or tap); L / R turn the character. Character selection: A enter the world, X create,
Y delete, SELECT change the realm, B back. Character creation: X random look, Y name, START create, B back.

## Files the client writes, reporting problems

All in `sdmc:/3ds/w3dproto/`: `pad.txt` (settings: `language 0` English, `1` Russian), `login.cfg` (server and account),
the hotbars per character, and with the performance log on (Settings) `perf.csv` and `perf_events.txt`. When something
is wrong in a place (falling through, hanging in the air, a wall where there is none), stand there for 5-10 seconds and
send `perf_events.txt`: every 3 seconds it records the height of the feet and what is under them. For converter
problems send the `log_*.txt` of the output folder.

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
`ui.w3dp`, the `sound/` folder and the text files the converter writes next to the packs, among them `areatriggers.txt` (without it dungeon doors and the Deeprun Tram do nothing) and `glue.txt` (texts, clothes of new characters and zone names of the character screens)), then start `w3dproto.3dsx`
through the Homebrew Launcher.

## Tests (no console and no game data needed)

Shared pool and `--all`: `converter/tests/run_pool_tests.sh <build dir>`.
Login / character screens: `proto3ds/tests/run_glue.sh [ui.w3dp] [glue.txt]` (screenshots and input checks).
Streaming order (a synthetic city read at SD-card speed): `proto3ds/tests/run_streaming.sh <build dir>`.
Particles and lamp glows: `proto3ds/tests/run_fx.sh [wmo.w3dp]` (after run_wmo_tests.sh).
Spell effects: `proto3ds/tests/run_spellfx.sh <build dir>`.
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
