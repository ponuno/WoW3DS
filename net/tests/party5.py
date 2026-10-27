"""Stage 5.17 part of the fake server (--party): a group invitation, the member list with one member in range and
one far away (party member stats), leadership, kicking, a failed invitation, leaving, and a declined invitation.

Independent Python implementation of the vanilla 1.12.1 server side, written from vMaNGOS WorldPackets::Group
(GroupInviteNotification, GroupList with the 1.11+ difficulty byte, GroupSetLeaderNotification, PartyCommandResult,
GroupDeclineNotification) and WorldSession::BuildPartyMemberStatsPacket (packed GUID, u32 mask, fields by mask).
"""
import struct

from stage5 import (World5, RESULTS5, SPAWN, create, values, update_packet, unit_fields, living_block, packguid)

RESULTS5.update({"party": {"accepts": 0, "declines": 0, "invites": [], "uninvites": [], "disbands": 0,
                           "stat_requests": [], "leaders": []}})
P = RESULTS5["party"]
FAR = 0x0000000000000123


class WorldParty(World5):
    def enter(self):
        me = self.me
        f = unit_fields(me, 0, 12, 300, 49); f[2] = 0x19; f[36] = 1 | (1 << 8); f[35] = 1
        self.send(0x0A9, update_packet([create(me, 4, living_block(0x01 | 0x10, 0, SPAWN, 3.07), f, two=True)]))
        of = unit_fields(self.other, 0, 11, 250, 50); of[2] = 0x19; of[36] = 1 | (2 << 8); of[35] = 1
        self.send(0x0A9, update_packet([create(self.other, 4, living_block(0x10, 0, (SPAWN[0] + 6, SPAWN[1], SPAWN[2]), 0.0), of)]))
        self.events = [(self.now() + 1.0, lambda: self.invite("Otherguy"))]
        self.members = []             # (name, guid, status)
        self.leader = 0
        self.round = 0
        RESULTS5["expected"]["party"] = True

    def invite(self, who):
        self.send(0x06F, who.encode() + b"\0")       # SMSG_GROUP_INVITE

    def group_list(self):
        b = struct.pack("<BBI", 0, 0, len(self.members))
        for name, g, st in self.members: b += name.encode() + b"\0" + struct.pack("<QBB", g, st, 0)
        b += struct.pack("<Q", self.leader)
        if self.members: b += struct.pack("<BQBB", 0, 0, 2, 0)   # loot method, master looter, threshold, difficulty
        self.send(0x07D, b)

    def stats(self, g, mask, hp, maxhp, level=20, full=False):
        b = packguid(g) + struct.pack("<I", mask)
        if mask & 0x1: b += bytes([1])
        if mask & 0x2: b += struct.pack("<H", hp)
        if mask & 0x4: b += struct.pack("<H", maxhp)
        if mask & 0x8: b += bytes([0])
        if mask & 0x10: b += struct.pack("<H", 400)
        if mask & 0x20: b += struct.pack("<H", 500)
        if mask & 0x40: b += struct.pack("<H", level)
        if mask & 0x80: b += struct.pack("<H", 12)
        if mask & 0x100: b += struct.pack("<hh", -9000, -100)
        if mask & 0x200: b += struct.pack("<I", 0b11) + struct.pack("<HH", 1243, 1459)   # two buffs
        if mask & 0x400: b += struct.pack("<H", 0)
        self.send(0x2F2 if full else 0x07E, b)

    def tick(self):
        t = self.now()
        due = [e for e in self.events if e[0] <= t]
        for e in due:
            self.events.remove(e); e[1]()

    def client_packet(self, op, body):
        if op == 0x072:                                # CMSG_GROUP_ACCEPT (no body)
            if body: self.err("GROUP_ACCEPT with a body")
            P["accepts"] += 1
            self.members = [("Otherguy", self.other, 1), ("Farguy", FAR, 1)]
            self.leader = self.other
            self.group_list()
            def damage(): self.stats(FAR, 0x2, 150, 0)                       # Farguy loses health (partial update)
            def promote():
                self.leader = self.me
                self.send(0x079, b"Testone\0")         # SMSG_GROUP_SET_LEADER
                self.group_list()
            self.events += [(self.now() + 0.6, damage), (self.now() + 1.2, promote)]
            return True
        if op == 0x073:                                # CMSG_GROUP_DECLINE
            if body: self.err("GROUP_DECLINE with a body")
            P["declines"] += 1
            return True
        if op == 0x27F:                                # CMSG_REQUEST_PARTY_MEMBER_STATS u64
            if len(body) != 8: self.err("REQUEST_PARTY_MEMBER_STATS size")
            g, = struct.unpack("<Q", body)
            P["stat_requests"].append(g)
            if g == FAR: self.stats(FAR, 0x7FF, 280, 300, full=True)
            elif g == self.other: self.stats(g, 0x7FF, 250, 260, level=11, full=True)
            else: self.err(f"stats request for {g:x}")
            return True
        if op == 0x06E:                                # CMSG_GROUP_INVITE name
            name = body.split(b"\0")[0].decode()
            if len(body) != len(name) + 1: self.err("GROUP_INVITE size")
            P["invites"].append(name)
            if name == "Nobody": self.send(0x07F, struct.pack("<I", 0) + b"Nobody\0" + struct.pack("<I", 1))
            return True
        if op == 0x075:                                # CMSG_GROUP_UNINVITE name
            name = body.split(b"\0")[0].decode()
            P["uninvites"].append(name)
            if self.leader != self.me: self.err("uninvite while not the leader")
            self.members = [m for m in self.members if m[0] != name]
            self.group_list()
            return True
        if op == 0x078:                                # CMSG_GROUP_SET_LEADER u64
            g, = struct.unpack("<Q", body)
            P["leaders"].append(g)
            return True
        if op == 0x07B:                                # CMSG_GROUP_DISBAND: we leave
            if body: self.err("GROUP_DISBAND with a body")
            P["disbands"] += 1
            self.members = []; self.leader = 0
            self.group_list()                          # empty list: not in a group any more
            self.events.append((self.now() + 0.5, lambda: self.invite("Pestguy")))
            return True
        return super().client_packet(op, body)
