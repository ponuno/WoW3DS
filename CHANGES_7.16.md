# 7.16 — items used on other items

- **A lure can be applied to the fishing pole.** Using Shiny Bauble (or another bait / lure) now sends the pole as the
  spell's item target, so the fishing bonus lands on the equipped pole. The same path covers weapon oils, sharpening
  stones and poisons used on a weapon.
- `w3dconv` writes the new `@item` lines into `spells.txt`: the spell's item class, subclass mask and inventory-type
  mask (`Spell.dbc`'s `EquippedItemClass` / `SubClassMask` / `InventoryTypeMask`) and whether the original client picks
  the item from the main hand (`SPELL_ATTR_HELD_ITEM_ONLY`). Spells are recognised by the `TARGET_FLAG_ITEM` bit of
  their `Targets` mask; when a spell leaves `Targets` empty, the held-item attribute plus an item-enchant effect
  (permanent / temporary / held item) identify it - so Fishing, which also names the main hand, is not one of them.
- The client sends `CMSG_USE_ITEM` with `TARGET_FLAG_ITEM` (0x10) and the packed GUID of the item, as vMaNGOS
  `SpellCastTargets` / `Spell::CheckCast` expect. A held-item spell always takes the main-hand item, like the original
  client; other item spells pick the first fitting worn item (or bag item).
- **Touch screen:** the Bags tab's **Use** applies the item to the single fitting target at once. If several items fit
  (for example a weapon oil with two weapons), it shows the items it can go on — worn ones first with their slot names —
  and the chosen one is the target. A hotbar slot uses the main-hand / first fitting item directly.
- Old packs keep working: without the `@item` lines the client behaves exactly as before. To get the new behaviour,
  re-run the text/interface step (`ui.bat` or `w3dconv --texts-only`) so `spells.txt` is written by this converter.

Tests: `converter/tests/run_text_tests.sh` (the synthetic Shiny Bauble spell → `@item 8087 2 1048576 0 1`) and
`net/tests/run_stage5.sh`'s `useitem_test` (main-hand selection, the item-target mask and GUID). Not checked on the
console yet.
