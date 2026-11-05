# 6.92 - 6.94 (cumulative over stage8zzy)
6.92 weapons as WoWee: sheath points 32/33/28/12, back matrix for 2H / bow / gun (worn_test).
6.93 draw / sheathe reach animation 89 / 90 (regenerate player models; anim_test +13).
6.94
- Map: the zone map of the player comes from the zone he stands in (audio::area() -> top parent), not the smallest overlapping
  WorldMapArea rectangle (w3dUiPickZoneMap in common/w3d_ui.h, map_zone_test); fallback = the old rule.
- Quest log (Quests tab, one quest): rewards (to choose / fixed items with icons, money); a tap shows the item card.
- Quest text (log + NPC quest window) scrolls by a finger drag (a line per 13 px); the arrow buttons stay.
- Item card: "To chat" button: the chat keyboard opens with |cff..|Hitem:ID:0:0:0|h[Name]|h|r at the start of the message.
Not built here: render.cpp, main.cpp (need the 3DS toolchain). ui_test builds on the PC but needs the recorded sessions of net/tests/run_stage5.sh to run.
