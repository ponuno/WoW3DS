# 7.14 — the converter takes the game's language

- **`--locale CODE`** for `w3dconv` (GUI: **Game language**). It selects the language of every text file the converter
  writes from DBC tables — `spells.txt` / `spelldesc.bin`, `talents.txt`, `skills.txt`, `taxi.txt`, `factions.txt`,
  `glue.txt` (zone names, GlueStrings.lua) — and prefers the client's `Data/<CODE>/` MPQ archives.
- Codes: `enUS` (default), `enGB`, `koKR`, `frFR`, `deDE`, `zhCN`, `zhTW`, `esES`, `esMX`, `ruRU`.
  `ruRU` is for fan clients that replace the English column; `enGB` is English.
- A DBC string group has eight columns in the game's order (`enUS`, `koKR`, `frFR`, `deDE`, `zhCN`, `zhTW`, `esES`,
  `esMX`). The converter now reads the selected column and falls back to the first translated one when a DBC lacks it.
  The old code always took the first non-empty column, which is English in a normal client.
- `--locale` is accepted by `--ui`, `--texts-only`, `--all`, `--sound` and the world/dungeon steps. The old English
  behaviour is unchanged when it is not given.
- Strings come out as UTF-8, as they are stored in the client. The 3DS UI's own labels (menus, buttons) are still
  English / Russian; this release localizes the game data, not the client chrome.

Tests: `converter/tests/run_text_tests.sh` (existing English round trips), `converter/tests/gui_core_test.cpp` (the
locale reaches every w3dconv step and survives `w3dgui.ini`), and a synthetic DBC check of the column selection.
