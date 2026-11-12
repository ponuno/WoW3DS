# 7.17 — Old 3DS controls: the D-pad as the hotbar

- **The schemes are named after the consoles in Settings:** *Forever* is now **New 3DS** and *Classic* is **Old 3DS**
  (Controls page). The layouts themselves are unchanged; `pad.txt` keeps `scheme 0` / `scheme 1`, so existing settings
  files read the same.
- **Old 3DS: Settings → Controls → D-pad → Camera / Spells.** The default is *Camera* (the old layout: D-pad left /
  right looks, up / down zooms). With *Spells* the D-pad becomes four hotbar cells of the set shown on the touch bar
  (`X` still changes the set), and the camera is looked with the **Circle Pad while Y is held** — the D-pad looks and
  zooms as before in that mode. A tap of Y without moving the Circle Pad still opens the chat, and L (next target), R
  (attack), A (interact), B (jump) and START keep their actions. Nothing changes unless the option is turned on.
- `pad.txt` gains `dpadspells 0|1`; it is written back with the rest of the controls settings.
- **The top-screen bars name the Old 3DS buttons by their real actions** (X *Set*, A *Use*, B *Jump*, Y *Chat* — *Look*
  with D-pad = Spells) instead of the New 3DS set-0 names; the binder's cell labels follow the same rules.
- **Code:** `pad.hpp` / `pad.cpp` (`PadConfig::dpadSpells`, `PadScheme::New3DS` / `Old3DS`, `Pad::new3ds` / `old3ds`),
  the controls page in `game_ui_tabs.cpp`, the settings action in `game_ui.cpp`. The Old 3DS hotbar set now lives in the
  `Pad` (`PadOut::set` for both schemes), so the top bars, the touch bar and the slots always agree.

Tests: the PC pad test (`proto3ds/tests/pad_test.cpp`, also run by `run_ui.sh`) now has 75 checks — the D-pad slots and
set switching, Y held to look vs. a Y tap for chat (a stale Y release after a pause is ignored), the D-pad camera while
Y is held, `dpadspells` in the `pad.txt` round trip — and passes; the 3DS build compiles and links (`make`,
`w3dproto.cia`). The Old 3DS mode was not tried on real hardware yet.
