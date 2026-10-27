"""Stage 5.10 part of the fake server (--life): inventory, item queries, the loot window, equip / use /
unequip / destroy, NPC gossip with NPC text, and three deaths with the three ways back to life
(run to the corpse, the spirit healer, another player's resurrection spell).

Independent Python implementation of the vanilla 1.12.1 server side, written from vMaNGOS:
  HandleItemQuerySingleOpcode (item data), WorldPackets::Item / Loot / Spell::UseItem, PlayerMenu::SendGossipMenu,
  NpcTextUpdate, Player::BuildPlayerRepop (ghost flag, corpse, reclaim delay, water walking, graveyard teleport),
  HandleReclaimCorpseOpcode (39 yd, delay), spirit healer (gossip -> SPIRIT_HEALER_CONFIRM -> ACTIVATE),
  ResurrectRequest / CMSG_RESURRECT_RESPONSE, MoveFlagChangeAck (u64 guid, counter, MovementInfo, u32 apply).
Update field indices from vMaNGOS UpdateFields_1_12_1.h (PLAYER_FLAGS 190, INV_SLOT_HEAD 486, COINAGE 1176,
ITEM stack 14 / durability 46,47, CONTAINER num slots 48 / slot 1 at 50, CORPSE owner 6).
"""
import math, struct, time

from stage5 import (World5, RESULTS5, SPAWN, CREATURE_GUID, create, values, update_packet, unit_fields,
                    living_block, position_block, packguid, movement_info, UF_SELF, UF_ALL, UF_HIGHGUID)

F_WATERWALK = 0x10000000
INV, COIN, PFLAGS = 486, 1176, 190
ITEM_HI, CORPSE_HI = 0x4000000000000000, 0xF101000000000000

# entry: (name, class, subclass, quality, inventory type, stack, container slots, on-use spell, durability, sell)
ITEMS = {
    25: ("Worn Shortsword", 2, 7, 1, 13, 1, 0, 0, 20, 7),
    38: ("Recruit's Shirt", 4, 0, 1, 4, 1, 0, 0, 0, 1),
    117: ("Tough Hunk of Bread", 0, 0, 1, 0, 20, 0, 433, 0, 1),
    6948: ("Hearthstone", 15, 0, 1, 0, 1, 0, 8690, 0, 0),
    6125: ("Brawler's Harness", 4, 1, 1, 5, 1, 0, 0, 25, 3),
    4496: ("Small Brown Pouch", 1, 0, 1, 18, 1, 6, 0, 0, 1),
    2589: ("Linen Cloth", 7, 0, 1, 0, 20, 0, 0, 0, 13),
    750: ("Tough Wolf Meat", 7, 0, 0, 0, 10, 0, 0, 0, 4),
}
ITEM_FLAGS = {}               # entry -> ItemPrototype Flags (0x4 ITEM_FLAG_LOOTABLE: "Right click to open")
MISSING_ITEM = 99999
ERR_CANT_BE_EQUIPPED = 20     # EQUIP_ERR_ITEM_CANT_BE_EQUIPPED (InventoryResult of build 5875)

RESULTS5.update({"life": {"item_queries": [], "loot": [], "equip": [], "use": [], "destroy": [], "unequip": [],
                          "gossip": [], "npc_text": 0, "repops": 0, "corpse_queries": 0, "reclaims": 0,
                          "healer": [], "res_response": [], "flag_acks": [], "ghost_moves": 0,
                          "ghost_moves_without_flag": 0, "alive_moves_with_flag": 0, "deaths": 0, "revived": []}})
L = RESULTS5["life"]


def item_query_response(entry):
    if entry not in ITEMS: return struct.pack("<I", entry | 0x80000000)
    name, cls, sub, q, inv, stack, slots, spell, dur, sell = ITEMS[entry]
    b = struct.pack("<III", entry, cls, sub) + name.encode() + b"\0" + b"\0\0\0"
    b += struct.pack("<IIIII", 1000 + entry, q, ITEM_FLAGS.get(entry, 0), sell * 4, sell)  # display, quality, flags, buy, sell
    b += struct.pack("<IIIII", inv, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0)                         # inv type, classes, races, ilvl, req lvl
    b += struct.pack("<7I", 0, 0, 0, 0, 0, 0, 0)                                          # skill .. rep rank
    b += struct.pack("<III", 0, stack, slots)                                             # max count, stackable, slots
    b += b"".join(struct.pack("<Ii", 0, 0) for _ in range(10))                           # stats
    for k in range(5):
        b += struct.pack("<ffI", 1.0, 3.0, 0) if (k == 0 and cls == 2) else struct.pack("<ffI", 0, 0, 0)
    b += struct.pack("<7I", 30 if cls == 4 else 0, 0, 0, 0, 0, 0, 0)                     # armor + 6 resistances
    b += struct.pack("<IIf", 1900 if cls == 2 else 0, 0, 0.0)                            # delay, ammo type, range mod
    for k in range(5):
        if k == 0 and spell: b += struct.pack("<IIiIII", spell, 0, -1, 0, 11, 1000)
        else: b += struct.pack("<IIiiIi", 0, 0, 0, -1, 0, -1)
    b += struct.pack("<I", 0) + b"A test item.\0"                                         # bonding, description
    b += struct.pack("<10I", *([0] * 10)) + struct.pack("<I", dur)                        # page text .. item set, max durability
    b += struct.pack("<III", 0, 0, 0)                                                     # area, map, bag family
    return b


class WorldLife(World5):
    def enter(self):
        me = self.me
        self.pos = (SPAWN[0], SPAWN[1], 3.07)
        self.next_low = 1
        self.items = {}            # guid -> [entry, count]
        self.slots = {}            # (bag, slot) -> guid; bag 255 = character, 19 = the pouch
        self.coinage = 1234
        f = unit_fields(me, 0, 5, 60, 49); f[2] = 0x19; f[716] = 10; f[717] = 400
        f[36] = 1 | (1 << 8); f[PFLAGS] = 0; f[193] = 0x03080601; f[194] = 0x02; f[COIN] = self.coinage
        blocks = []
        def add(bag, slot, entry, count=1, container=False):
            g = ITEM_HI | (500 + self.next_low); self.next_low += 1
            self.items[g] = [entry, count]; self.slots[(bag, slot)] = g
            fl = {0: g & 0xFFFFFFFF, 1: g >> 32, 2: 0x7 if container else 0x3, 3: entry, 4: 1.0, 6: me & 0xFFFFFFFF, 7: me >> 32,
                  8: me & 0xFFFFFFFF, 9: me >> 32, 14: count, 46: ITEMS[entry][8], 47: ITEMS[entry][8]}
            if container: fl[48] = ITEMS[entry][6]
            return g, fl
        layout = [(255, 15, 25, 1), (255, 3, 38, 1), (255, 23, 117, 4), (255, 24, 6948, 1), (255, 25, 6125, 1)]
        for bag, slot, entry, count in layout:
            g, fl = add(bag, slot, entry, count)
            blocks.append(create(g, 1, b"\x10" + struct.pack("<I", 1), fl))
            f[INV + 2 * slot] = g & 0xFFFFFFFF; f[INV + 2 * slot + 1] = g >> 32
        pouch, pf = add(255, 19, 4496, 1, container=True)
        f[INV + 38] = pouch & 0xFFFFFFFF; f[INV + 39] = pouch >> 32
        linen, lf = add(19, 0, 2589, 2)
        lf[8] = pouch & 0xFFFFFFFF; lf[9] = pouch >> 32
        pf[50] = linen & 0xFFFFFFFF; pf[51] = linen >> 32
        blocks.append(create(pouch, 2, b"\x10" + struct.pack("<I", 1), pf))
        blocks.append(create(linen, 1, b"\x10" + struct.pack("<I", 1), lf))
        blocks.insert(0, create(me, 4, living_block(UF_SELF | UF_ALL, 0, SPAWN, 3.07), f, two=True))
        # a dead, lootable wolf at our feet and Marshal McBride (gossip + quest giver) 3.6 yd away
        self.wolf = CREATURE_GUID(69, 300)
        wf = unit_fields(self.wolf, 69, 3, 0, 669); wf[143] = 1
        blocks.append(create(self.wolf, 3, living_block(UF_HIGHGUID, 0, (SPAWN[0] + 2, SPAWN[1] + 1, SPAWN[2]), 1.0), wf))
        self.marshal = CREATURE_GUID(197, 5)
        mf = unit_fields(self.marshal, 197, 10, 500, 3258); mf[147] = 0x3
        blocks.append(create(self.marshal, 3, living_block(UF_HIGHGUID, 0, (SPAWN[0] + 3, SPAWN[1] - 2, SPAWN[2]), 1.0), mf))
        self.send(0x0A9, update_packet(blocks))
        of = unit_fields(self.other, 0, 7, 140, 50); of[2] = 0x19
        self.send(0x0A9, update_packet([create(self.other, 4, living_block(UF_ALL, 0, (SPAWN[0] + 15, SPAWN[1], SPAWN[2]), 0.0), of)]))
        self.events = []
        self.dead = False; self.ghost = False; self.death = 0
        self.corpse = None; self.reclaim_at = 0
        self.healer = None
        self.pending_flag = None   # (ack opcode, counter, apply)
        self.loot_open = False; self.loot_items = {}
        RESULTS5["expected"]["life"] = True

    # ---- helpers
    def inv_values(self, extra=None):
        f = {}
        for s in range(39):
            g = self.slots.get((255, s), 0)
            f[INV + 2 * s] = g & 0xFFFFFFFF; f[INV + 2 * s + 1] = g >> 32
        f[COIN] = self.coinage
        if extra: f.update(extra)
        return values(self.me, f)
    def pouch_values(self):
        pouch = self.slots[(255, 19)]
        f = {}
        for i in range(6):
            g = self.slots.get((19, i), 0)
            f[50 + 2 * i] = g & 0xFFFFFFFF; f[51 + 2 * i] = g >> 32
        return values(pouch, f)
    def free_backpack(self):
        return next(s for s in range(23, 39) if (255, s) not in self.slots)
    def new_item(self, entry, count):
        g = ITEM_HI | (500 + self.next_low); self.next_low += 1
        self.items[g] = [entry, count]
        dur = ITEMS.get(entry, ("", 0, 0, 0, 0, 1, 0, 0, 0, 0))[8]
        fl = {0: g & 0xFFFFFFFF, 1: g >> 32, 2: 0x3, 3: entry, 4: 1.0, 6: self.me & 0xFFFFFFFF, 7: self.me >> 32,
              8: self.me & 0xFFFFFFFF, 9: self.me >> 32, 14: count, 46: dur, 47: dur}
        return g, create(g, 1, b"\x10" + struct.pack("<I", 1), fl)
    def flag_change(self, op, ack, apply):
        self.counter += 1
        self.pending_flag = (ack, self.counter, apply)
        self.send(op, packguid(self.me) + struct.pack("<I", self.counter))
    def force_speed(self, speed):
        self.counter += 1
        self.expect_ack[0xE3] = self.counter
        self.want_speed = speed
        self.send(0x0E2, packguid(self.me) + struct.pack("<If", self.counter, speed))
    def teleport_to(self, pos):
        self.counter += 1
        self.tele_pos = pos
        RESULTS5["teleport_ack"] = False; RESULTS5["after_teleport_ok"] = False
        self.expect_ack[0xC7] = self.counter
        self.send(0x0C7, packguid(self.me) + struct.pack("<I", self.counter) + movement_info(0, int(self.now() * 1000), *pos, 0.5))
    def kill_me(self):
        self.death += 1; L["deaths"] = self.death
        self.dead = True
        self.death_pos = (self.pos[0], self.pos[1], SPAWN[2])
        self.send(0x0A9, update_packet([values(self.me, {22: 0})]))
        if self.death == 3:
            self.events.append((self.now() + 1.0, self.offer_resurrect))
    def offer_resurrect(self):
        name = b"Otherguy"
        self.send(0x15B, struct.pack("<QI", self.other, len(name) + 1) + name + b"\0" + struct.pack("<BB", 0, 0))
    def revive(self, how, hp):
        self.dead = self.ghost = False
        L["revived"].append(how)
        self.send(0x0A9, update_packet([values(self.me, {22: hp, PFLAGS: 0})]))
        if how != "resurrect":
            self.flag_change(0x0DF, 0x2D0, 0)          # SMSG_MOVE_LAND_WALK
            self.force_speed(7.0)
        if self.corpse:
            self.send(0x0AA, struct.pack("<Q", self.corpse)); self.corpse = None
        if self.death < 3: self.events.append((self.now() + 0.7, self.kill_me))

    def tick(self):
        t = self.now()
        due = [e for e in self.events if e[0] <= t]
        for e in due:
            self.events.remove(e); e[1]()

    # ---- client packets
    def client_packet(self, op, body):
        if op in self.MOVE_OPS and RESULTS5["active_mover"]:
            if len(body) >= 4:
                flags, = struct.unpack_from("<I", body, 0)
                if self.ghost and self.pending_flag is None:
                    L["ghost_moves"] += 1
                    if not flags & F_WATERWALK: L["ghost_moves_without_flag"] += 1
                if not self.ghost and not self.dead and self.pending_flag is None and flags & F_WATERWALK:
                    L["alive_moves_with_flag"] += 1
            if self.dead and not self.ghost and len(body) >= 4 and struct.unpack_from("<I", body)[0] & 0xF:
                self.err("moving while dead (before releasing the spirit)")
            return super().client_packet(op, body)
        if op == 0xE3 and 0xE3 in self.expect_ack:     # speed ACK with the speed we asked for (ghost 8.75, alive 7)
            g, cnt = struct.unpack_from("<QI", body, 0)
            if g != self.me or cnt != self.expect_ack.pop(0xE3): self.err("speed ACK guid/counter")
            (flags, *_), end = self.parse_mi(body, 12)
            sp, = struct.unpack_from("<f", body, end)
            if abs(sp - self.want_speed) > 1e-4 or len(body) != end + 4: self.err(f"speed ACK {sp} size {len(body)}")
            self.speed = sp
            return True
        if op in (0x2D0, 0x2CF, 0x0F6):                # movement flag change ACK
            if not self.pending_flag: self.err(f"unexpected flag ACK 0x{op:X}"); return True
            ack, cnt, apply = self.pending_flag
            g, c = struct.unpack_from("<QI", body, 0)
            (flags, *_), end = self.parse_mi(body, 12)
            a, = struct.unpack_from("<I", body, end)
            ok = op == ack and g == self.me and c == cnt and a == apply and len(body) == end + 4 and \
                 bool(flags & F_WATERWALK) == bool(apply)
            if not ok: self.err(f"flag ACK 0x{op:X} guid {g:x} counter {c}/{cnt} apply {a}/{apply} flags {flags:x} size {len(body)}")
            L["flag_acks"].append([op, a])
            self.pending_flag = None
            return True
        if op == 0x13D:                                # CMSG_SET_SELECTION
            RESULTS5["selection"] = struct.unpack("<Q", body)[0]; return True
        if op == 0x056:                                # CMSG_ITEM_QUERY_SINGLE: u32 entry, u64 guid
            if len(body) != 12: self.err(f"ITEM_QUERY size {len(body)}")
            entry, = struct.unpack_from("<I", body)
            L["item_queries"].append(entry)
            self.send(0x058, item_query_response(entry))
            return True
        if op == 0x15D:                                # CMSG_LOOT
            g, = struct.unpack("<Q", body)
            if g != self.wolf or math.dist(self.pos[:2], (SPAWN[0] + 2, SPAWN[1] + 1)) > 5.0:
                self.err("loot of something else / too far"); self.send(0x160, struct.pack("<QBB", g, 0, 4)); return True
            self.loot_open = True
            self.loot_money = 50
            self.loot_items = {0: (2589, 1), 2: (750, 1), 3: (MISSING_ITEM, 1)}
            b = struct.pack("<QBIB", g, 1, self.loot_money, len(self.loot_items))
            for slot, (e, n) in self.loot_items.items(): b += struct.pack("<BIIIIIB", slot, e, n, 1000 + e, 0, 0, 0)
            self.send(0x160, b)
            L["loot"].append("open"); return True
        if op == 0x108:                                # CMSG_AUTOSTORE_LOOT_ITEM: u8 slot
            if len(body) != 1 or not self.loot_open: self.err("AUTOSTORE_LOOT_ITEM size / closed"); return True
            slot = body[0]
            if slot not in self.loot_items: self.err(f"loot slot {slot} not there"); return True
            if not L["loot"][1:] and slot != 2: self.err("the first item taken should be the meat (slot 2)")
            e, n = self.loot_items.pop(slot)
            L["loot"].append(f"item{slot}")
            self.send(0x162, bytes([slot]))
            g, blk = self.new_item(e, n)
            s = self.free_backpack(); self.slots[(255, s)] = g
            self.send(0x0A9, update_packet([blk, self.inv_values()]))
            self.send(0x166, struct.pack("<QIIIBIIIII", self.me, 0, 0, 1, 255, s, e, 0, 0, n))
            return True
        if op == 0x15E:
            if body or not self.loot_open: self.err("LOOT_MONEY body / closed")
            self.coinage += self.loot_money; self.loot_money = 0
            self.send(0x165, b""); self.send(0x0A9, update_packet([self.inv_values()]))
            L["loot"].append("money"); return True
        if op == 0x15F:
            g, = struct.unpack("<Q", body)
            self.loot_open = False
            L["loot"].append("release"); self.send(0x161, struct.pack("<QB", g, 1)); return True
        if op == 0x10A:                                # CMSG_AUTOEQUIP_ITEM: u8 bag, u8 slot
            if len(body) != 2: self.err("AUTOEQUIP size"); return True
            bag, slot = body
            g = self.slots.get((bag, slot))
            L["equip"].append([bag, slot])
            if not g: self.send(0x112, struct.pack("<BQQB", 22, 0, 0, 0)); return True
            entry = self.items[g][0]
            inv = ITEMS[entry][4]
            if inv == 0:
                self.send(0x112, struct.pack("<BQQB", ERR_CANT_BE_EQUIPPED, g, 0, 0)); return True
            dest = {5: 4, 4: 3, 13: 15, 18: 19}.get(inv)
            old = self.slots.pop((255, dest), None)
            del self.slots[(bag, slot)]
            self.slots[(255, dest)] = g
            if old: self.slots[(bag, slot)] = old
            self.send(0x0A9, update_packet([self.inv_values()]))
            return True
        if op == 0x0AB:                                # CMSG_USE_ITEM: u8 bag, u8 slot, u8 spell slot, u16 target mask
            if len(body) != 5: self.err(f"USE_ITEM size {len(body)}"); return True
            bag, slot, spell_slot, mask = struct.unpack("<BBBH", body)
            g = self.slots.get((bag, slot))
            L["use"].append([bag, slot, spell_slot, mask])
            if not g or mask != 0: self.err("USE_ITEM of nothing / with a target"); return True
            self.items[g][1] -= 1
            self.send(0x0A9, update_packet([values(g, {14: self.items[g][1]})]))
            return True
        if op == 0x111:                                # CMSG_DESTROYITEM: u8 bag, u8 slot, u8 count, 3 x u8
            if len(body) != 6: self.err(f"DESTROYITEM size {len(body)}"); return True
            bag, slot, count = body[:3]
            g = self.slots.pop((bag, slot), None)
            L["destroy"].append([bag, slot, count])
            if not g: self.err("destroy of an empty slot"); return True
            if count != self.items[g][1]: self.err(f"destroy count {count} != stack {self.items[g][1]}")
            del self.items[g]
            self.send(0x0A9, update_packet([self.pouch_values() if bag == 19 else self.inv_values()]))
            self.send(0x0AA, struct.pack("<Q", g))
            return True
        if op == 0x10B:                                # CMSG_AUTOSTORE_BAG_ITEM: src bag, src slot, dst bag
            if len(body) != 3: self.err("AUTOSTORE_BAG_ITEM size"); return True
            sb, ss, db = body
            L["unequip"].append([sb, ss, db])
            g = self.slots.pop((sb, ss), None)
            if not g or db != 255: self.err("unequip of nothing / not into the bags"); return True
            self.slots[(255, self.free_backpack())] = g
            self.send(0x0A9, update_packet([self.inv_values()]))
            return True
        if op == 0x17B:                                # CMSG_GOSSIP_HELLO: u64
            g, = struct.unpack("<Q", body)
            L["gossip"].append(["hello", g & 0xFFFFFF])
            if g == self.marshal:
                if self.dead or self.ghost: self.err("talking to the marshal while dead")
                b = struct.pack("<QII", g, 68, 2)
                b += struct.pack("<IBB", 0, 0, 0) + b"Tell me about Northshire.\0"
                b += struct.pack("<IBB", 1, 3, 0) + b"Train me.\0"
                b += struct.pack("<I", 1) + struct.pack("<III", 783, 2, 1) + b"A Threat Within\0"
                self.send(0x17D, b)
            elif g == self.healer:
                if not self.ghost: self.err("spirit healer gossip while alive")
                self.send(0x17D, struct.pack("<QII", g, 0, 1) + struct.pack("<IBB", 0, 4, 0) + b"Return me to life.\0" + struct.pack("<I", 0))
            else:
                self.err(f"gossip with {g:x}")
            return True
        if op == 0x17F:                                # CMSG_NPC_TEXT_QUERY: u32 text, u64 guid
            tid, g = struct.unpack("<IQ", body)
            L["npc_text"] += 1
            b = struct.pack("<I", tid)
            for i in range(8):
                txt = b"Greetings, $N. Stay alert.$BThe kobolds are near.\0" if i == 0 else b"\0"
                b += struct.pack("<f", 1.0 if i == 0 else 0.0) + txt + b"\0" + struct.pack("<I", 0) + b"\0" * 24
            self.send(0x180, b)
            return True
        if op == 0x17C:                                # CMSG_GOSSIP_SELECT_OPTION: u64 guid, u32 option
            if len(body) != 12: self.err(f"GOSSIP_SELECT size {len(body)}")
            g, opt = struct.unpack_from("<QI", body)
            L["gossip"].append(["select", g & 0xFFFFFF, opt])
            if g == self.marshal:
                self.send(0x17E, b"")                  # SMSG_GOSSIP_COMPLETE
                self.events.append((self.now() + 0.7, self.kill_me))   # first death
            elif g == self.healer:
                self.send(0x222, struct.pack("<Q", g))   # spell 17251: SMSG_SPIRIT_HEALER_CONFIRM
            return True
        if op == 0x15A:                                # CMSG_REPOP_REQUEST: no body
            if body: self.err("REPOP_REQUEST with a body")
            if not self.dead or self.ghost: self.err("repop while alive / already a ghost"); return True
            L["repops"] += 1
            self.ghost = True
            self.corpse = CORPSE_HI | self.death
            cf = {0: self.corpse & 0xFFFFFFFF, 1: self.corpse >> 32, 2: 0x81, 3: 0, 4: 1.0, 6: self.me & 0xFFFFFFFF, 7: self.me >> 32}
            self.send(0x0A9, update_packet([values(self.me, {22: 1, PFLAGS: 0x10}),
                                            create(self.corpse, 7, position_block(UF_ALL, self.death_pos, 0.0), cf)]))
            self.reclaim_at = self.now() + 2.0
            self.send(0x269, struct.pack("<I", 2000))   # SMSG_CORPSE_RECLAIM_DELAY
            self.force_speed(8.75)
            self.flag_change(0x0DE, 0x2D0, 1)           # SMSG_MOVE_WATER_WALK
            if self.death == 1:
                gy = (SPAWN[0] + 40, SPAWN[1] + 30, SPAWN[2])   # 50 yd from the body: must walk >= 11 yd back
            else:
                gy = (SPAWN[0] - 30, SPAWN[1] + 20, SPAWN[2])
                self.healer = CREATURE_GUID(6491, 400)
                hf = unit_fields(self.healer, 6491, 60, 4000, 5233); hf[147] = 0x21
                self.send(0x0A9, update_packet([create(self.healer, 3, living_block(UF_HIGHGUID, 0, (gy[0] + 3, gy[1], gy[2]), 3.1), hf)]))
            self.teleport_to(gy)
            return True
        if op == 0x216:                                # MSG_CORPSE_QUERY: no body
            L["corpse_queries"] += 1
            if self.corpse: self.send(0x216, struct.pack("<BifffI", 1, 0, *self.death_pos, 0))
            else: self.send(0x216, b"\0")
            return True
        if op == 0x1D2:                                # CMSG_RECLAIM_CORPSE: u64
            if len(body) != 8: self.err("RECLAIM size")
            L["reclaims"] += 1
            d = math.dist(self.pos[:2], self.death_pos[:2])
            if not self.ghost or d > 39 or self.now() < self.reclaim_at:
                self.err(f"reclaim not allowed (ghost {self.ghost}, {d:.1f} yd, delay {self.reclaim_at - self.now():.1f} s)"); return True
            self.revive("corpse", 30)
            return True
        if op == 0x21C:                                # CMSG_SPIRIT_HEALER_ACTIVATE: u64
            g, = struct.unpack("<Q", body)
            L["healer"].append(g & 0xFFFFFF)
            if g != self.healer or not self.ghost: self.err("spirit healer activate: wrong guid / not a ghost"); return True
            self.send(0x2BD, b"")                       # SMSG_DURABILITY_DAMAGE_DEATH
            self.revive("healer", 30)
            return True
        if op == 0x15C:                                # CMSG_RESURRECT_RESPONSE: u64, u8
            g, acc = struct.unpack("<QB", body)
            L["res_response"].append([g & 0xFFFFFF, acc])
            if g != self.other or acc != 1 or len(body) != 9: self.err("RESURRECT_RESPONSE"); return True
            self.revive("resurrect", 20)
            return True
        return super().client_packet(op, body)
