questdb.bin - where the objectives of quests are (6.63). Copy it to the SD card: sdmc:/3ds/w3dproto/questdb.bin

Made by questie_import.py from the Classic data of Questie's database (github.com/Questie/QuestieDB, data/Classic/*.lua):
    python3 questie_import.py <QuestieDB>/data/Classic questdb.bin
It holds 4100 vanilla quests (Questie's spawns of the creatures / objects a quest wants, and of what drops the items it wants, as
blobs in zone percent; the client turns them into world coordinates with the zone maps of ui.w3dp). Quests Questie does not know
(Turtle WoW's own) are followed in the tracker but have no area on the map.
Questie is GPL-3 (see its LICENSE); this file is derived from its data.

6.76: the areas are no longer one big disc per group of spawns: questie_import.py now writes the spawns themselves, thinned on a grid
(one small dot per occupied cell, at most ~60 per objective), so an area follows where the mobs / objects really are. Same file format.

6.85 (version 2): tdb_import.py makes the same file from the Forever database (the world SQL of advocaite/TrinityCore, release
forever-tdb-2026-10-14), with world coordinates instead of zone percent, so no zone maps are needed:
    python3 tdb_import.py TDB_full_world_forever_2026_10_14.sql questdb.bin
It holds 4219 quests of quest_template_classic_level (3847 of them are also in the Questie file, 372 are new Forever quests, 253 Questie
quests - mostly Turtle WoW's - are not in it). The client reads both versions (header word 1 = zone percent, 2 = world yards) and draws the
same areas; copy the file you want to sdmc:/3ds/w3dproto/questdb.bin. The Questie file stays the better one for a 1.12 server, the
Forever one is for a Forever server. TrinityCore data is GPL-2.0-or-later; this file is derived from it.
