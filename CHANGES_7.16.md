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
- **Touch screen:** the Bags tab's **Use** on a use-on-item item opens a picker of the items it fits — worn ones first
  with their slot names — and the chosen one is the target. The list only holds compatible items: a lure shows the worn
  fishing pole, a weapon oil the weapons its spell allows. A hotbar slot opens the same picker on the Bags page.
- **The GUI lists only the languages the client really has.** **Check available languages** runs
  `w3dconv --list-locales`: it reads `Spell.dbc`'s text columns and the `Data\<code>\` folders, fills the dropdown with
  those languages only and names them under it (`Available languages: enUS, ruRU`). The client's own
  `WTF\Config.wtf` language is preselected when it is in the list; on a new/old settings file the first scan takes it
  once, afterwards the user's choice is remembered. Only the selected language is converted — the list never means
  "convert all of them".
- **Item cards show the enchant and its time left.** `w3dconv` appends `@enchant id name` lines to `spells.txt` from
  `SpellItemEnchantment.dbc`, and the client reads `ITEM_FIELD_ENCHANTMENT` (7 slots of id / time left / charges) plus
  `SMSG_ITEM_ENCHANT_TIME_UPDATE` (0x1EB). A pole with Shiny Bauble now says `Enchant: Fishing Lure +25 (9m 59s left)`
  and the time counts down while the card is open. Permanent enchants and charges (poisons) are shown too; vendor / loot
  items without an instance simply have no enchant lines.
- Old packs keep working: without the `@item` lines the client behaves exactly as before. To get the new behaviour,
  re-run the text/interface step (`ui.bat` or `w3dconv --texts-only`) so `spells.txt` is written by this converter.

Tests: `converter/tests/run_text_tests.sh` (the synthetic Shiny Bauble spell → `@item 8087 2 1048576 0 1` and
`@enchant 263 Fishing Lure +25`) and `net/tests/run_stage5.sh`'s `useitem_test` (main-hand selection, the item-target
mask and GUID, ITEM_FIELD_ENCHANTMENT, the time-update countdown and the tooltip line). `run_text_tests.sh` also checks
`--list-locales` (English DBC → `enUS`, Spanish columns → `enUS` + `esES`). The GUI was launched against the real
Stonetavern client: the dropdown held only `English (enUS)` and `Russian (ruRU)` and the line read
`Available languages: enUS, ruRU`. Not checked on the console yet.
