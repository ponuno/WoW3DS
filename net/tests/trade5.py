"""Stages 5.12-5.14 part of the fake server (--trade): faction reactions, quests and a vendor.

Independent Python implementation of the vanilla 1.12.1 server side, written from vMaNGOS:
  ReputationMgr::SendInitialReputations / SendState (WorldPackets::Misc::InitializeFactions, SetFactionStanding),
  QuestHandler.cpp + WorldPackets::Quest (Quest.cpp: QuestGiverStatus, QuestGiverQuestList, QuestGiverQuestDetails,
  QuestGiverRequestItems, QuestGiverOfferReward, QuestGiverQuestComplete, QuestQueryResponse, QuestUpdateAddKill,
  QuestUpdateAddItem, QuestUpdateComplete), Player::SetQuestSlot / SetQuestSlotCounter / SetQuestSlotState,
  PlayerMenu::SendGossipMenu / CloseGossip, WorldSession::SendListInventory, Player::BuyItemFromVendor,
  HandleSellItemOpcode, HandleBuybackItem, HandleRepairItemOpcode (WorldPackets::Item / Npc).
The faction data of the test is in data/factions_test.txt (a hand-made factions.txt).
"""
import math, struct

from stage5 import (RESULTS5, SPAWN, CREATURE_GUID, create, values, update_packet, unit_fields, living_block, packguid,
                    position_block, UF_ALL)
from life5 import WorldLife, ITEMS, ITEM_FLAGS, item_query_response, ITEM_HI, INV, COIN

QLOG = 198                      # PLAYER_QUEST_LOG_1_1
BUYBACK_GUID, BUYBACK_PRICE = 0x270, 0x4CA
ITEMS.update({
    159: ("Refreshing Spring Water", 0, 0, 1, 0, 20, 0, 430, 0, 1),
    2947: ("Small Throwing Knife", 2, 16, 1, 25, 200, 0, 0, 0, 1),
    999: ("Gilded Ceremonial Blade", 2, 7, 3, 13, 1, 0, 0, 50, 50000),
    5405: ("Draped Cloak", 4, 1, 2, 16, 1, 0, 0, 20, 30),
    11119: ("Milly's Harvest", 12, 0, 1, 0, 10, 0, 0, 0, 0),
    5523: ("Small Barnacled Clam", 15, 0, 1, 0, 20, 0, 0, 0, 3),
    5503: ("Clam Meat", 7, 0, 1, 0, 20, 0, 0, 0, 2),
    1394: ("Kobold Hunter Gloves", 4, 2, 2, 10, 1, 0, 0, 15, 25),
})

ITEM_FLAGS[5523] = 0x4         # a clam: opened with CMSG_OPEN_ITEM
RESULTS5.update({"trade": {"opened": [],"status_queries": {}, "status_answers": [], "attack_swings": 0, "quest_queries": [], "aura_cancels": [],
                           "trainer_lists": 0, "trained": [], "binder": 0, "go_use": [], "go_loot": [],
                           "quest_flow": [], "hello": 0, "cancels": 0, "removed": [], "buys": [], "sells": [],
                           "buybacks": [], "repairs": [], "list_inventory": 0, "money_end": None}})
T = RESULTS5["trade"]

QUESTS = {   # id: (title, level, objectives text, details, kills [(entry, count)], items [(entry, count)], money, xp, choices)
    7: ("Kobold Camp Cleanup", 3, "Kill 2 Kobold Vermin and bring a piece of Tough Wolf Meat to Deputy Willem.",
        "Your first task, $N, is clearing out the kobolds.$BBe careful.", [(6, 2)], [(750, 1)], 35, 120, [5405, 1394]),
    33: ("Wolves Across the Border", 2, "Bring 8 Tough Wolf Meat to Eagan Peltskinner.", "The wolves grow bold.",
         [], [(750, 8)], 10, 80, []),
    783: ("A Threat Within", 1, "Speak with Marshal McBride.", "The marshal awaits you.", [], [], 0, 40, []),
    176: ("Wanted: Hogger", 10, "Bring Hogger's claw.", "A huge gnoll...", [], [], 0, 400, []),
}


def quest_query_response(qid):
    title, lvl, obj, det, kills, items, money, xp, choices = QUESTS[qid]
    b = struct.pack("<IIIII", qid, 2, lvl, 9, 0)                # id, method, level, zone (Northshire), type
    b += struct.pack("<IIII", 0, 0, 0, 0) + struct.pack("<I", 0)  # rep objective, opposite rep, next in chain
    b += struct.pack("<IIIII", money, 0, 0, 0, 0)               # money, money at max level, spell, source item, flags
    b += b"".join(struct.pack("<II", 0, 0) for _ in range(4))
    ch = choices + [0] * (6 - len(choices))
    b += b"".join(struct.pack("<II", c, 1 if c else 0) for c in ch)
    b += struct.pack("<IffI", 0, 0.0, 0.0, 0)
    b += title.encode() + b"\0" + obj.encode() + b"\0" + det.encode() + b"\0" + b"Return to Deputy Willem.\0"
    objs = []
    for k in range(4):
        e, c = kills[k] if k < len(kills) else (0, 0)
        it, ic = items[k] if k < len(items) else (0, 0)
        objs.append(struct.pack("<IIII", e, c, it, ic))
    b += b"".join(objs)
    b += b"".join(((b"Kobold Vermin slain\0" if k == 0 and kills else b"\0")) for k in range(4))
    return b


def item_lines(ids, counts=None):
    out = struct.pack("<I", len(ids))
    for i, e in enumerate(ids): out += struct.pack("<III", e, counts[i] if counts else 1, 1000 + e)
    return out


class WorldTrade(WorldLife):
    def enter(self):
        me = self.me
        self.pos = (SPAWN[0], SPAWN[1], 3.07)
        self.next_low = 1
        self.items, self.slots = {}, {}
        self.coinage = 1500
        f = unit_fields(me, 0, 5, 120, 49); f[2] = 0x19; f[716] = 10; f[717] = 400
        f[35] = 1                                           # FACTIONTEMPLATE: human player
        f[36] = 1 | (1 << 8); f[190] = 0; f[193] = 0x03080601; f[194] = 0x02; f[COIN] = self.coinage
        # stage 5.16: a cancelable buff in slot 0 and a debuff in slot 32 (SpellAuraHolder::SetAura / SetAuraFlag)
        f[47] = 1127; f[95] = 0x9; f[47 + 32] = 6788
        blocks = []
        def add(bag, slot, entry, count=1, dur=None):
            g = ITEM_HI | (700 + self.next_low); self.next_low += 1
            self.items[g] = [entry, count]; self.slots[(bag, slot)] = g
            mx = ITEMS[entry][8]
            fl = {0: g & 0xFFFFFFFF, 1: g >> 32, 2: 0x3, 3: entry, 4: 1.0, 6: me & 0xFFFFFFFF, 7: me >> 32,
                  8: me & 0xFFFFFFFF, 9: me >> 32, 14: count, 46: mx if dur is None else dur, 47: mx}
            blocks.append(create(g, 1, b"\x10" + struct.pack("<I", 1), fl))
            f[INV + 2 * slot] = g & 0xFFFFFFFF; f[INV + 2 * slot + 1] = g >> 32
            return g
        self.sword = add(255, 15, 25, 1, dur=10)             # worn sword: 10/20, repair costs 15c
        self.shirt = add(255, 25, 38)                      # a spare shirt in the backpack (this vendor refuses it)
        self.hearth = add(255, 23, 6948)
        self.linen = add(255, 24, 2589, 2)
        self.clam = add(255, 26, 5523, 1)
        self.meat = None
        blocks.insert(0, create(me, 4, living_block(0x01 | 0x10, 0, SPAWN, 3.07), f, two=True))
        # units: (entry, low, npc flags, faction template, unit flags, dx, dy, level)
        self.units = {}
        def unit(name, entry, low, npc, tmpl, uflags, dx, dy, lvl=5):
            g = CREATURE_GUID(entry, low)
            uf = unit_fields(g, entry, lvl, 100, 300 + entry); uf[147] = npc; uf[35] = tmpl; uf[46] = uflags
            blocks.append(create(g, 3, living_block(0x08, 0, (SPAWN[0] + dx, SPAWN[1] + dy, SPAWN[2]), 1.0), uf))
            self.units[name] = g
            return g
        self.willem = unit("Deputy Willem", 823, 1, 0x3, 12, 0, 3, -2)
        self.mcbride = unit("Marshal McBride", 197, 2, 0x2, 12, 0, 3, 2)
        self.danil = unit("Brother Danil", 152, 3, 0x4005, 12, 0, -3, 0)
        unit("Stormwind City Guard", 68, 4, 0, 12, 0, 6, 6, 55)
        wolf = unit("Young Wolf", 299, 5, 0, 32, 0, 20, 0)
        unit("Kobold Vermin", 6, 6, 0, 32, 0, 22, 3)
        unit("Rabbit", 721, 7, 0, 31, 0, 15, 5, 1)
        unit("Defias Bandit", 116, 8, 0, 17, 0, 25, -6)
        unit("Booty Bay Bruiser", 4624, 9, 0, 120, 0, 9, -9, 55)
        unit("Invisible Stalker", 15214, 10, 0, 32, 0x02000000, 4, 8)
        self.trainer = unit("Llane Beshere", 911, 11, 0x11, 12, 0, -3, 3)     # gossip | trainer
        self.inn = unit("Innkeeper Farley", 295, 12, 0x81, 12, 0, -3, -3)    # gossip | innkeeper
        self.learned = set()
        # stage 5.18: a quest chest (type 3, GO_DYNFLAG_LO_ACTIVATE) and a wanted poster (quest giver object, type 2)
        GO_HI = 0xF110000000000000
        self.chest = GO_HI | (161557 << 24) | 1
        self.poster = GO_HI | (68 << 24) | 2
        for g, entry, typ, dyn, (dx, dy) in ((self.chest, 161557, 3, 1, (2, 4)), (self.poster, 68, 2, 0, (-2, -5))):
            gf = {0: g & 0xFFFFFFFF, 1: g >> 32, 2: 0x21, 3: entry, 4: 1.0, 8: 300 + typ, 14: 1, 19: dyn, 21: typ}
            blocks.append(create(g, 5, position_block(UF_ALL, (SPAWN[0] + dx, SPAWN[1] + dy, SPAWN[2]), 0.0), gf))
        self.send(0x0A9, update_packet(blocks))
        self.send(0x137, struct.pack("<BI", 0, 1800000))     # SMSG_UPDATE_AURA_DURATION: 30 min of Well Fed
        # the wolf: Rend x3 on it (slot 32, AURAAPPLICATIONS byte = stacks - 1) and its target is us
        self.send(0x0A9, update_packet([values(wolf, {47 + 32: 772, 113 + 8: 2, 16: self.me & 0xFFFFFFFF, 17: self.me >> 32})]))
        # two Horde players: one not flagged for PvP, one flagged
        self.horde = 0x77; self.pvp = 0x78
        for g, flags in ((self.horde, 0), (self.pvp, 0x1000)):
            of = unit_fields(g, 0, 7, 140, 51); of[2] = 0x19; of[35] = 2; of[36] = 2 | (1 << 8); of[46] = flags
            self.send(0x0A9, update_packet([create(g, 4, living_block(0x10, 0, (SPAWN[0] - 12, SPAWN[1] + (g - 0x77) * 3, SPAWN[2]), 0.0), of)]))
        # the reputation list: Stormwind (7) visible, Defias Brotherhood (13) at war, Booty Bay (20) visible +2000
        reps = [(0, 0)] * 64
        reps[7] = (0x01, 0); reps[13] = (0x03, 0); reps[20] = (0x01, 2000)
        self.send(0x122, struct.pack("<I", 64) + b"".join(struct.pack("<Bi", fl, st) for fl, st in reps))
        self.standing = {7: 0, 13: 0, 20: 2000}
        self.qlog = [0] * 20          # quest id per slot
        self.qcount = [0] * 20        # packed counters + state
        self.done = set()
        self.events = []
        self.dead = self.ghost = False
        self.pending_flag = None
        self.vendor_stock = {2947: 2}
        self.buyback = {}             # slot index 0..11 -> (guid, price)
        RESULTS5["expected"]["trade"] = True

    # ---- helpers
    def money(self, delta):
        self.coinage += delta
        self.send(0x0A9, update_packet([values(self.me, {COIN: self.coinage})]))
    def qlog_values(self):
        f = {}
        for s in range(20):
            f[QLOG + 3 * s] = self.qlog[s]; f[QLOG + 3 * s + 1] = self.qcount[s]; f[QLOG + 3 * s + 2] = 0
        return values(self.me, f)
    def slot_of(self, qid): return self.qlog.index(qid) if qid in self.qlog else -1
    def status_for(self, g):
        if g == self.willem:
            s = self.slot_of(7)
            if 7 in self.done: return 0
            if s < 0: return 5
            return 4 if self.qcount[s] >> 24 & 1 else 3
        if g == self.mcbride:
            return 5 if (33 not in self.qlog) else 3
        if g == self.poster:
            return 5
        return 0
    def give_item(self, entry, count):
        g, blk = self.new_item(entry, count)
        s = self.free_backpack(); self.slots[(255, s)] = g
        self.send(0x0A9, update_packet([blk, self.inv_values()]))
        self.send(0x166, struct.pack("<QIIIBIIIII", self.me, 0, 0, 1, 255, s, entry, 0, 0, count))
        return g
    def take_item(self, g):
        for k, v in list(self.slots.items()):
            if v == g: del self.slots[k]
        self.items.pop(g, None)
        self.send(0x0A9, update_packet([self.inv_values()]))
        self.send(0x0AA, struct.pack("<Q", g))
    def kill_progress(self, n):
        s = self.slot_of(7)
        self.qcount[s] = (self.qcount[s] & ~0x3F) | n
        self.send(0x199, struct.pack("<IIIIQ", 7, 6, n, 2, CREATURE_GUID(6, 50 + n)))   # SMSG_QUESTUPDATE_ADD_KILL
        self.send(0x0A9, update_packet([self.qlog_values()]))
    def meat_drop(self):
        self.send(0x19A, struct.pack("<II", 750, 1))        # ADD_ITEM before the item itself, as vMaNGOS does
        self.meat = self.give_item(750, 1)
        s = self.slot_of(7)
        self.qcount[s] |= 1 << 24                            # QUEST_STATE_COMPLETE in byte 3
        self.send(0x198, struct.pack("<I", 7))               # SMSG_QUESTUPDATE_COMPLETE
        self.send(0x0A9, update_packet([self.qlog_values()]))
    def gossip(self, g, text_id, options, quests):
        b = struct.pack("<QII", g, text_id, len(options))
        for i, (icon, text) in enumerate(options): b += struct.pack("<IBB", i, icon, 0) + text.encode() + b"\0"
        b += struct.pack("<I", len(quests))
        for qid, icon in quests: b += struct.pack("<III", qid, icon, QUESTS[qid][1]) + QUESTS[qid][0].encode() + b"\0"
        self.send(0x17D, b)
    def details(self, g, qid):
        title, lvl, obj, det, kills, items, money, xp, choices = QUESTS[qid]
        b = struct.pack("<QI", g, qid) + title.encode() + b"\0" + det.encode() + b"\0" + obj.encode() + b"\0"
        b += struct.pack("<I", 1) + item_lines(choices) + item_lines([]) + struct.pack("<II", money, 0)
        b += struct.pack("<I", 4) + b"".join(struct.pack("<II", 0, 0) for _ in range(4))
        self.send(0x188, b)
    def set_standing(self, idx, delta):
        self.standing[idx] += delta
        self.send(0x124, struct.pack("<I", 1) + struct.pack("<Ii", idx, self.standing[idx]))

    def tick(self):
        t = self.now()
        due = [e for e in self.events if e[0] <= t]
        for e in due:
            self.events.remove(e); e[1]()

    # ---- client packets
    def client_packet(self, op, body):
        if op == 0x060:                                # creature query: names of this scenario
            entry, g = struct.unpack("<IQ", body)
            names = {911: ("Llane Beshere", "Warrior Trainer"), 295: ("Innkeeper Farley", "Innkeeper"), 823: ("Deputy Willem", ""), 197: ("Marshal McBride", ""), 152: ("Brother Danil", "General Supplies"),
                     68: ("Stormwind City Guard", ""), 299: ("Young Wolf", ""), 6: ("Kobold Vermin", ""), 721: ("Rabbit", ""),
                     116: ("Defias Bandit", ""), 4624: ("Booty Bay Bruiser", ""), 15214: ("Invisible Stalker", "")}
            n, sub = names.get(entry, ("?", ""))
            self.send(0x061, struct.pack("<I", entry) + n.encode() + b"\0\0\0\0" + sub.encode() + b"\0" + struct.pack("<IIIIIII", 0, 1, 0, 0, 0, 0, 600 + entry) + b"\0\0")
            return True
        if op == 0x050:
            g, = struct.unpack("<Q", body)
            name = {self.horde: "Hordeguy", self.pvp: "Pvpguy"}.get(g, "Unknown")
            self.send(0x051, struct.pack("<Q", g) + name.encode() + b"\0\0" + struct.pack("<III", 2, 0, 1))
            return True
        if op == 0x141:                                # CMSG_ATTACKSWING: the client must not try the guard
            T["attack_swings"] += 1
            self.err("ATTACKSWING sent (the client should have refused a friendly target)")
            self.send(0x149, b"")
            return True
        if op == 0x05E:                                # CMSG_GAMEOBJECT_QUERY: names of this scenario
            entry, g = struct.unpack("<IQ", body)
            name, typ = {161557: ("Milly's Harvest", 3), 68: ("Wanted Poster", 2)}.get(entry, ("Mailbox", 19))
            self.send(0x05F, struct.pack("<III", entry, typ, 300 + typ) + name.encode() + b"\0\0\0\0" + b"\0" * 24)
            return True
        if op == 0x0B1:                                # CMSG_GAMEOBJ_USE u64
            if len(body) != 8: self.err("GAMEOBJ_USE size"); return True
            g, = struct.unpack("<Q", body)
            T["go_use"].append("chest" if g == self.chest else "poster" if g == self.poster else f"{g:x}")
            if g == self.chest:                        # a loot window on the object (SMSG_LOOT_RESPONSE, type 1 corpse/chest)
                self.loot_src = (g, 11119)
                self.send(0x160, struct.pack("<QBIB", g, 1, 0, 1) + struct.pack("<BIIIIIB", 0, 11119, 1, 1000 + 11119, 0, 0, 0))
            elif g == self.poster:
                b = struct.pack("<Q", g) + b"WANTED: Hogger\0" + struct.pack("<II", 0, 0) + bytes([1])
                b += struct.pack("<III", 176, 5, 10) + b"Wanted: Hogger\0"
                self.send(0x185, b)
            else:
                self.err(f"use of object {g:x}")
            return True
        if op == 0x0AC:                                # CMSG_OPEN_ITEM u8 bag, u8 slot
            if len(body) != 2: self.err("OPEN_ITEM size"); return True
            bag, slot = body
            g = self.slots.get((bag, slot))
            T["opened"].append([bag, slot])
            if g != self.clam: self.err("opened something else than the clam"); return True
            self.loot_src = (g, 5503)                  # Player::SendLoot on the item
            self.send(0x160, struct.pack("<QBIB", g, 1, 0, 1) + struct.pack("<BIIIIIB", 0, 5503, 1, 1000 + 5503, 0, 0, 0))
            return True
        if op == 0x108:                                # CMSG_AUTOSTORE_LOOT_ITEM (the chest / the clam)
            src, entry = self.loot_src
            T["go_loot"].append(body[0])
            self.send(0x162, bytes([body[0]]))
            self.give_item(entry, 1)
            return True
        if op == 0x15F:                                # CMSG_LOOT_RELEASE
            g, = struct.unpack("<Q", body)
            T["go_loot"].append("release" if g == self.chest else "release clam" if g == self.clam else f"release {g:x}")
            self.send(0x161, struct.pack("<QB", g, 1))
            if g == self.clam: self.take_item(self.clam)   # an emptied clam is gone
            return True
        if op == 0x182:                                # CMSG_QUESTGIVER_STATUS_QUERY u64
            if len(body) != 8: self.err("QUESTGIVER_STATUS_QUERY size")
            g, = struct.unpack("<Q", body)
            key = "willem" if g == self.willem else "mcbride" if g == self.mcbride else "poster" if g == self.poster else f"{g:x}"
            T["status_queries"][key] = T["status_queries"].get(key, 0) + 1
            if g not in (self.willem, self.mcbride, self.poster): self.err(f"status query for a non quest giver {g:x}")
            st = self.status_for(g)
            if g == self.willem: T["status_answers"].append(st)
            self.send(0x183, struct.pack("<QI", g, st))
            return True
        if op == 0x05C:                                # CMSG_QUEST_QUERY u32
            if len(body) != 4: self.err("QUEST_QUERY size")
            qid, = struct.unpack("<I", body)
            T["quest_queries"].append(qid)
            if qid in QUESTS: self.send(0x05D, quest_query_response(qid))
            return True
        if op == 0x17B:                                # CMSG_GOSSIP_HELLO
            g, = struct.unpack("<Q", body)
            if g == self.willem:
                st = self.status_for(g)
                quests = [] if st == 0 else [(7, st)]
                self.gossip(g, 1, [], quests)
            elif g == self.danil:
                self.gossip(g, 2, [(1, "Let me browse your goods."), (0, "Tell me about the abbey.")], [])
            elif g == self.mcbride:
                self.err("GOSSIP_HELLO for a quest giver without the gossip flag (the client sends QUESTGIVER_HELLO)")
            elif g == self.trainer:
                self.gossip(g, 3, [(3, "I require warrior training.")], [])
            elif g == self.inn:
                self.gossip(g, 4, [(5, "Make this inn your home."), (1, "I want to browse your goods.")], [])
            else:
                self.err(f"gossip with {g:x}")
            return True
        if op == 0x17C:                                # CMSG_GOSSIP_SELECT_OPTION
            g, opt = struct.unpack_from("<QI", body)
            if g == self.danil and opt == 0:
                self.list_inventory(g)
            elif g == self.trainer and opt == 0:
                self.trainer_list(g)
            elif g == self.inn and opt == 0:
                self.send(0x17E, b"")                    # CloseGossip, then the confirmation (Player::SetBindPoint)
                self.send(0x2EB, struct.pack("<Q", g))
            else:
                self.err(f"gossip option {opt} of {g:x}")
            return True
        if op == 0x184:                                # CMSG_QUESTGIVER_HELLO
            g, = struct.unpack("<Q", body)
            T["hello"] += 1
            if g != self.mcbride: self.err("QUESTGIVER_HELLO to someone else")
            b = struct.pack("<Q", g) + b"Greetings, $N. Northshire needs you.\0" + struct.pack("<II", 0, 0)
            qs = [(33, 5), (783, 5)] if 33 not in self.qlog else [(33, 3), (783, 5)]
            b += bytes([len(qs)]) + b"".join(struct.pack("<III", q, ic, QUESTS[q][1]) + QUESTS[q][0].encode() + b"\0" for q, ic in qs)
            self.send(0x185, b)
            return True
        if op == 0x186:                                # CMSG_QUESTGIVER_QUERY_QUEST u64, u32
            if len(body) != 12: self.err("QUERY_QUEST size")
            g, qid = struct.unpack("<QI", body)
            T["quest_flow"].append(["query", qid])
            if self.slot_of(qid) >= 0: self.err(f"QUERY_QUEST for quest {qid} in the log (should ask for completion)")
            self.details(g, qid)
            return True
        if op == 0x189:                                # CMSG_QUESTGIVER_ACCEPT_QUEST u64, u32
            if len(body) != 12: self.err("ACCEPT_QUEST size")
            g, qid = struct.unpack("<QI", body)
            T["quest_flow"].append(["accept", qid])
            s = self.qlog.index(0)
            self.qlog[s] = qid; self.qcount[s] = 0
            self.send(0x0A9, update_packet([self.qlog_values()]))
            self.send(0x17E, b"")                       # CloseGossip -> SMSG_GOSSIP_COMPLETE
            if qid == 7:
                self.events.append((self.now() + 0.6, lambda: self.kill_progress(1)))
                self.events.append((self.now() + 0.9, lambda: self.kill_progress(2)))
                self.events.append((self.now() + 1.2, self.meat_drop))
            return True
        if op == 0x18A:                                # CMSG_QUESTGIVER_COMPLETE_QUEST u64, u32
            if len(body) != 12: self.err("COMPLETE_QUEST size")
            g, qid = struct.unpack("<QI", body)
            T["quest_flow"].append(["complete", qid])
            s = self.slot_of(qid)
            done = s >= 0 and bool(self.qcount[s] >> 24 & 1)
            b = struct.pack("<QI", g, qid) + QUESTS[qid][0].encode() + b"\0" + b"Do you have the meat, $N?\0"
            b += struct.pack("<IIII", 0, 0, 0, 0) + item_lines([750]) + struct.pack("<IIII", 2, 3 if done else 0, 4, 8)
            self.send(0x18B, b)
            return True
        if op == 0x18C:                                # CMSG_QUESTGIVER_REQUEST_REWARD u64, u32
            g, qid = struct.unpack("<QI", body)
            T["quest_flow"].append(["reward?", qid])
            title, lvl, obj, det, kills, items, money, xp, choices = QUESTS[qid]
            b = struct.pack("<QI", g, qid) + title.encode() + b"\0" + b"Well done, $N. Take one of these.\0"
            b += struct.pack("<II", 1, 1) + struct.pack("<II", 0, 1)            # auto finish, 1 emote (delay, emote)
            b += item_lines(choices) + item_lines([]) + struct.pack("<III", money, 0, 0)
            self.send(0x18D, b)
            return True
        if op == 0x18E:                                # CMSG_QUESTGIVER_CHOOSE_REWARD u64, u32, u32
            if len(body) != 16: self.err("CHOOSE_REWARD size")
            g, qid, choice = struct.unpack("<QII", body)
            T["quest_flow"].append(["choose", qid, choice])
            s = self.slot_of(qid)
            title, lvl, obj, det, kills, items, money, xp, choices = QUESTS[qid]
            self.qlog[s] = 0; self.qcount[s] = 0; self.done.add(qid)
            if self.meat: self.take_item(self.meat); self.meat = None
            self.give_item(choices[choice], 1)
            self.coinage += money
            self.send(0x0A9, update_packet([self.qlog_values(), values(self.me, {COIN: self.coinage})]))
            self.send(0x191, struct.pack("<IIIII", qid, 3, xp, money, 1) + struct.pack("<II", choices[choice], 1))
            self.set_standing(7, 250)
            self.set_standing(20, 1500)
            return True
        if op == 0x190:                                # CMSG_QUESTGIVER_CANCEL
            if body: self.err("QUESTGIVER_CANCEL with a body")
            T["cancels"] += 1
            self.send(0x17E, b"")
            return True
        if op == 0x194:                                # CMSG_QUESTLOG_REMOVE_QUEST u8
            if len(body) != 1: self.err("REMOVE_QUEST size")
            s = body[0]
            T["removed"].append([s, self.qlog[s]])
            self.qlog[s] = 0; self.qcount[s] = 0
            self.send(0x0A9, update_packet([self.qlog_values()]))
            return True
        # ---- vendor
        if op == 0x19E:                                # CMSG_LIST_INVENTORY u64
            g, = struct.unpack("<Q", body)
            T["list_inventory"] += 1
            self.list_inventory(g)
            return True
        if op == 0x1A2:                                # CMSG_BUY_ITEM u64, u32 item, u8 count, u8 0
            if len(body) != 14: self.err(f"BUY_ITEM size {len(body)}"); return True
            g, entry, count, z = struct.unpack("<QIBB", body)
            T["buys"].append([entry, count])
            price, bc = {159: (25, 5), 2947: (15, 1), 999: (1000000, 1)}.get(entry, (0, 0))
            if g != self.danil or not bc: self.err("buy from someone else / unknown item"); return True
            if entry in self.vendor_stock and self.vendor_stock[entry] < count:
                self.err("the client bought a sold-out item"); self.send(0x1A5, struct.pack("<QIB", g, entry, 7)); return True
            if price * count > self.coinage:
                self.err("the client tried to buy what it cannot pay"); self.send(0x1A5, struct.pack("<QIB", g, entry, 2)); return True
            self.money(-price * count)
            self.give_item(entry, bc * count)
            slot = {159: 1, 2947: 2, 999: 3}[entry]
            stock = 0xFFFFFFFF
            if entry in self.vendor_stock:
                self.vendor_stock[entry] -= count; stock = self.vendor_stock[entry]
            self.send(0x1A4, struct.pack("<QIII", g, slot, stock, bc * count))
            return True
        if op == 0x1A0:                                # CMSG_SELL_ITEM u64, u64, u8 count
            if len(body) != 17: self.err(f"SELL_ITEM size {len(body)}"); return True
            g, item, count = struct.unpack("<QQB", body)
            entry = self.items.get(item, [0])[0]
            T["sells"].append([entry, count])
            if item == self.shirt:                     # this merchant does not want shirts
                self.send(0x1A1, struct.pack("<QQB", g, item, 2)); return True
            if item not in self.items or ITEMS[entry][9] == 0: self.err("sold an unsellable / unknown item"); return True
            n = self.items[item][1] if count == 0 else count
            value = ITEMS[entry][9] * n
            for k, v in list(self.slots.items()):
                if v == item: del self.slots[k]
            self.buyback[0] = (item, value)
            self.coinage += value
            self.send(0x0A9, update_packet([self.inv_values({BUYBACK_GUID: item & 0xFFFFFFFF, BUYBACK_GUID + 1: item >> 32,
                                                             BUYBACK_PRICE: value})]))
            return True
        if op == 0x290:                                # CMSG_BUYBACK_ITEM u64, u32 slot
            if len(body) != 12: self.err("BUYBACK size"); return True
            g, slot = struct.unpack("<QI", body)
            T["buybacks"].append(slot)
            if slot != 69 or 0 not in self.buyback: self.err(f"buyback slot {slot}"); return True
            item, price = self.buyback.pop(0)
            s = self.free_backpack(); self.slots[(255, s)] = item
            self.coinage -= price
            self.send(0x0A9, update_packet([self.inv_values({BUYBACK_GUID: 0, BUYBACK_GUID + 1: 0, BUYBACK_PRICE: 0})]))
            return True
        if op == 0x136:                                # CMSG_CANCEL_AURA u32 spell
            if len(body) != 4: self.err("CANCEL_AURA size"); return True
            sp, = struct.unpack("<I", body)
            T["aura_cancels"].append(sp)
            if sp != 1127: self.err(f"cancel of aura {sp}"); return True
            self.send(0x0A9, update_packet([values(self.me, {47: 0, 95: 0})]))
            return True
        if op == 0x1B0:                                # CMSG_TRAINER_LIST u64
            g, = struct.unpack("<Q", body)
            self.trainer_list(g); return True
        if op == 0x1B2:                                # CMSG_TRAINER_BUY_SPELL u64, u32
            if len(body) != 12: self.err("TRAINER_BUY_SPELL size"); return True
            g, sp = struct.unpack("<QI", body)
            T["trained"].append(sp)
            if g != self.trainer or sp != 7380 or sp in self.learned: self.err(f"training {sp} not allowed"); return True
            self.learned.add(sp)
            self.money(-10)
            self.send(0x12B, struct.pack("<Hh", 284, 0))   # SMSG_LEARNED_SPELL: the taught Heroic Strike rank 2
            self.send(0x1B3, struct.pack("<QI", g, sp))     # SMSG_TRAINER_BUY_SUCCEEDED
            return True
        if op == 0x1B5:                                # CMSG_BINDER_ACTIVATE u64
            if len(body) != 8: self.err("BINDER_ACTIVATE size"); return True
            g, = struct.unpack("<Q", body)
            T["binder"] += 1
            if g != self.inn: self.err("binder activate for someone else"); return True
            self.send(0x155, struct.pack("<fffII", SPAWN[0], SPAWN[1], SPAWN[2], 0, 9))   # SMSG_BINDPOINTUPDATE
            return True
        if op == 0x2A8:                                # CMSG_REPAIR_ITEM u64 npc, u64 item
            if len(body) != 16: self.err("REPAIR size"); return True
            g, item = struct.unpack("<QQ", body)
            T["repairs"].append(item)
            if g != self.danil or item != 0: self.err("repair: not the vendor / not 'all'"); return True
            self.coinage -= 15
            self.send(0x0A9, update_packet([values(self.sword, {46: 20}), values(self.me, {COIN: self.coinage})]))
            return True
        return super().client_packet(op, body)

    def trainer_list(self, g):
        T["trainer_lists"] += 1
        rows = [(7380, 2 if 7380 in self.learned else 0, 10, 4, 78), (3127, 1, 100, 6, 0), (2457, 2, 0, 1, 0)]
        b = struct.pack("<QII", g, 0, len(rows))
        for sp, state, cost, lvl, req in rows:
            b += struct.pack("<IBIIIBIIIII", sp, state, cost, 0, 0, lvl, 0, 0, req, 0, 0)
        b += b"Hello, warrior! Ready for some training?\0"
        self.send(0x1B1, b)

    def list_inventory(self, g):
        rows = [(159, 25, 0xFFFFFFFF, 5), (2947, 15, self.vendor_stock[2947], 1), (999, 1000000, 0xFFFFFFFF, 1)]
        b = struct.pack("<QB", g, len(rows))
        for i, (e, price, stock, bc) in enumerate(rows):
            b += struct.pack("<IIIIIII", i + 1, e, 1000 + e, stock, price, ITEMS[e][8], bc)
        self.send(0x19F, b)

    def finish(self):
        T["money_end"] = self.coinage
        super().finish()
