questdb.bin - where the objectives of quests are (6.63). Copy it to the SD card: sdmc:/3ds/w3dproto/questdb.bin

Made by questie_import.py from the Classic data of Questie's database (github.com/Questie/QuestieDB, data/Classic/*.lua):
    python3 questie_import.py <QuestieDB>/data/Classic questdb.bin
It holds 4100 vanilla quests (Questie's spawns of the creatures / objects a quest wants, and of what drops the items it wants, as
blobs in zone percent; the client turns them into world coordinates with the zone maps of ui.w3dp). Quests Questie does not know
(Turtle WoW's own) are followed in the tracker but have no area on the map.
Questie is GPL-3 (see its LICENSE); this file is derived from its data.

6.76: the areas are no longer one big disc per group of spawns: questie_import.py now writes the spawns themselves, thinned on a grid
(one small dot per occupied cell, at most ~60 per objective), so an area follows where the mobs / objects really are. Same file format.

License / credit: questdb.bin is derived from Questie's Classic database (https://github.com/Questie/Questie, GPL-3.0) and is
distributed under GPL-3.0 with credit to the Questie authors and contributors. questie_import.py (the script that makes it) is GPL-3.0 too.
