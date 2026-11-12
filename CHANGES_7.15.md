# 7.15 — the client's interface in five languages

- **Settings → Interface → Language** now cycles **English → Русский → Español → Deutsch → Français**. The choice is
  saved in `pad.txt` (`language 0..4`: English, Russian, Spanish, German, French).
- The UI strings are translated without touching the 700+ `tr("English", "Русский")` call sites: English stays the key
  and `tr()` looks the other languages up in `lang_esES.txt`, `lang_deDE.txt`, `lang_frFR.txt`. Russian is still the
  built-in second argument.
- The three tables are bundled in the application's romfs and are also read from `sdmc:/3ds/w3dproto/lang_<code>.txt`
  when present (so a translation can be fixed on the SD card without rebuilding). Format: one `English<TAB>translation`
  line; `\n` and `\t` are the two-character escapes.
- Covered: every `tr()` label (tabs, windows, items, spells, talents, quests, vendor, trainer, mail, auction, bank,
  trade, pet, death, login and character screens), slot names, compass directions, chat quick phrases, race/class names,
  PvP ranks, money/date suffixes, and known server lines through `trText`.
- The machine translation is a first pass (especially the long hint lines); correcting a line in the `lang_*.txt`
  files is enough — no code change. Names of races/classes use the official game words where possible.
- **Accents:** the converter now bakes the whole Latin-1 range (`á é í ó ú ñ ü ß ç à ...`) into the interface fonts.
  Re-run `ui.bat` / `w3dconv --ui` with this release to get them; an old `ui.w3dp` has no such glyphs. Without `ui.w3dp`
  the built-in stub font still covers only ASCII/Cyrillic, so accented letters may be missing there.
- Note: this is **UI language**, separate from the converter's `--locale` game-data language. The two can differ; for a
  Spanish game, use `--locale esES` in the converter and Spanish in the client Settings.

Tests: `proto3ds/tests/lang_test.cpp` (table loading, escaped newlines/spaces, fallback, the bundled Spanish file);
`run_ui.sh` builds and runs it. Built with devkitARM; `w3dproto.3dsx` / `w3dproto.cia` include the language files.
Not checked on the console yet.
