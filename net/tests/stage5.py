"""Stage-5 part of the fake server: an independent (Python) implementation of
the vanilla 1.12.1 server side of SMSG_UPDATE_OBJECT, the compressed variants,
SMSG_MONSTER_MOVE, relayed player movement and forced-movement ACKs, plus a
strict checker for the client's own movement packets.

Layouts written from vMaNGOS (Object::BuildMovementUpdate, MovementInfo::Write,
PacketBuilder::WriteMonsterMove), not from the client code.
"""
import math, struct, sys, time, zlib

FIGHT = "--fight" in sys.argv               # chat/target/melee/loot scenario instead of the movement tests

SPAWN = (-8949.95, -132.49, 83.53)          # Northshire Abbey
RESULTS5 = {"errors": [], "move_packets": 0, "heartbeats": 0, "facings": 0, "starts": 0, "stops": 0,
            "speed_ack": False, "teleport_ack": False, "root_ack": False, "unroot_ack": False,
            "after_teleport_ok": False, "max_speed_seen": 0.0, "name_queries": 0, "creature_queries": 0,
            "go_queries": 0, "expected": {},
            # --fight (stage 5.6/5.7): chat, target, melee, xp, loot
            "chat_in": [], "selection": None, "swings": 0, "attack_errors": [], "attack_stops": 0,
            "wolf_dead": False, "loot": [], "lang": None,
            # stage 5.8: spells
            "casts": [], "cancels": 0,
            "active_mover": 0, "moves_before_mover": 0}

# movement flags (vMaNGOS MovementFlags)
F_FORWARD, F_BACKWARD, F_SL, F_SR = 0x1, 0x2, 0x4, 0x8
F_ROOT, F_JUMPING, F_SWIMMING, F_SPLINE = 0x1000, 0x2000, 0x200000, 0x400000
F_TRANSPORT, F_SPLINE_ELEV = 0x2000000, 0x4000000
UF_SELF, UF_TRANSPORT, UF_MELEE, UF_HIGHGUID, UF_ALL, UF_LIVING, UF_POS = 1, 2, 4, 8, 0x10, 0x20, 0x40

def packguid(g):
    mask, out = 0, b""
    for i in range(8):
        b = (g >> (8 * i)) & 0xFF
        if b: mask |= 1 << i; out += bytes([b])
    return bytes([mask]) + out

def fields_blob(fields):                       # {index: u32 or float}
    if not fields: return b"\0"
    nblocks = max(fields) // 32 + 1
    masks = [0] * nblocks
    for i in fields: masks[i // 32] |= 1 << (i % 32)
    out = bytes([nblocks]) + b"".join(struct.pack("<I", m) for m in masks)
    for i in sorted(fields):
        v = fields[i]
        out += struct.pack("<f", v) if isinstance(v, float) else struct.pack("<I", v & 0xFFFFFFFF)
    return out

def movement_info(flags, t, x, y, z, o, pitch=0.0, fall=0, jump=None, transport=None, elev=None):
    out = struct.pack("<IIffff", flags, t, x, y, z, o)
    if flags & F_TRANSPORT:
        g, tx, ty, tz, to = transport
        out += packguid(g) + struct.pack("<ffff", tx, ty, tz, to)
    if flags & F_SWIMMING: out += struct.pack("<f", pitch)
    out += struct.pack("<I", fall)
    if flags & F_JUMPING: out += struct.pack("<ffff", *jump)
    if flags & F_SPLINE_ELEV: out += struct.pack("<f", elev)
    return out

def living_block(uflags, mflags, pos, o, spline=None, **kw):
    out = bytes([uflags | UF_LIVING]) + movement_info(mflags, 1000, *pos, o, **kw)
    out += struct.pack("<ffffff", 2.5, 7.0, 4.5, 4.722222, 2.5, 3.141594)
    if mflags & F_SPLINE:
        sflags, final, passed, dur, pts, end = spline
        out += struct.pack("<I", sflags)
        if sflags & 0x10000: out += struct.pack("<fff", *final)
        elif sflags & 0x20000: out += struct.pack("<Q", final)
        elif sflags & 0x40000: out += struct.pack("<f", final)
        out += struct.pack("<IIII", passed, dur, 77, len(pts)) + b"".join(struct.pack("<fff", *p) for p in pts)
        out += struct.pack("<fff", *end)
    if uflags & UF_HIGHGUID: out += struct.pack("<I", 0)
    if uflags & UF_ALL: out += struct.pack("<I", 1)
    if uflags & UF_MELEE: out += packguid(1)
    if uflags & UF_TRANSPORT: out += struct.pack("<I", 12345)
    return out

def position_block(uflags, pos, o):
    out = bytes([uflags | UF_POS]) + struct.pack("<ffff", *pos, o)
    if uflags & UF_HIGHGUID: out += struct.pack("<I", 0)
    if uflags & UF_ALL: out += struct.pack("<I", 1)
    return out

def create(guid, typeid, movement, fields, two=False):
    return bytes([3 if two else 2]) + packguid(guid) + bytes([typeid]) + movement + fields_blob(fields)

def values(guid, fields):
    return b"\x00" + packguid(guid) + fields_blob(fields)

def movement_upd(guid, movement):
    return b"\x01" + struct.pack("<Q", guid) + movement

def out_of_range(guids):
    return b"\x04" + struct.pack("<I", len(guids)) + b"".join(packguid(g) for g in guids)

def update_packet(blocks):
    return struct.pack("<IB", len(blocks), 0) + b"".join(blocks)

def unit_fields(guid, entry, level, hp, display):
    return {0: guid & 0xFFFFFFFF, 1: guid >> 32, 2: 0x9, 3: entry, 4: 1.0, 22: hp, 28: hp + 10, 34: level,
            35: 7, 131: display, 132: display, 129: 0.5, 130: 1.5}

def pack_xyz(dx, dy, dz):                      # mangos appendPackXYZ
    x = int(dx / 0.25) & 0x7FF; y = int(dy / 0.25) & 0x7FF; z = int(dz / 0.25) & 0x3FF
    return x | (y << 11) | (z << 22)

def monster_move(guid, start, dest, mids, dur, flags=0x100, uncompressed_pts=None, kind=0, facing=None):
    out = packguid(guid) + struct.pack("<fffI", *start, 1234) + bytes([kind])
    if kind == 1: return out
    if kind == 4: out += struct.pack("<f", facing)
    out += struct.pack("<II", flags, dur)
    if uncompressed_pts is not None:
        out += struct.pack("<I", len(uncompressed_pts)) + b"".join(struct.pack("<fff", *p) for p in uncompressed_pts)
    else:
        out += struct.pack("<I", len(mids) + 1) + struct.pack("<fff", *dest)
        mid = [(start[i] + dest[i]) / 2 for i in range(3)]
        for p in mids: out += struct.pack("<I", pack_xyz(mid[0] - p[0], mid[1] - p[1], mid[2] - p[2]))
    return out

CREATURE_GUID = lambda entry, low: 0xF130000000000000 | (entry << 24) | low

class World5:
    def __init__(self, send, self_guid):
        self.send, self.me = send, self_guid
        self.t0 = time.time()
        self.last = None              # (t, x, y, z, flags) of the client's last movement packet
        self.counter = 0
        self.events = []              # (time offset, callable)
        self.expect_ack = {}
        self.tele_pos = None
        self.speed = 7.0
        self.other = 0x0000000000000099
        self.other_pos = [SPAWN[0] + 15, SPAWN[1], SPAWN[2]]
        self.rooted = False
        self.last_flags = 0

    def now(self): return time.time() - self.t0

    def enter(self):
        exp = RESULTS5["expected"]
        me = self.me
        blocks = []
        # self: CREATE_OBJECT2, SELF | ALL | LIVING, full player fields (incl. a high index)
        f = unit_fields(me, 0, 1, 60, 49); f[2] = 0x19; f[716] = 10; f[717] = 400; f[1281] = 5
        f[36] = 1 | (1 << 8) | (0 << 16)          # UNIT_FIELD_BYTES_0: human warrior, male
        self.my_hp = 60
        blocks.append(create(me, 4, living_block(UF_SELF | UF_ALL, 0, SPAWN, 3.07), f, two=True))
        # items in the bags (type 1) - must be stored but not drawn
        for i in range(6):
            blocks.append(create(0x4000000000000000 | (100 + i), 1, b"\x10" + struct.pack("<I", 1),
                                 {0: 100 + i, 1: 0x40000000, 2: 0x3, 3: 25 + i, 4: 1.0}))
        # a few units near the abbey: idle, one with a spline running, one swimming, one jumping,
        # one attacking (MELEE flag) and one on a transport
        units = []
        units.append((CREATURE_GUID(299, 1), 299, SPAWN[0] + 10, SPAWN[1] + 4, 0, None, {}))   # Young Wolf
        sp = (0x100, None, 500, 4000, [(SPAWN[0] + 20, SPAWN[1], SPAWN[2]), (SPAWN[0] + 24, SPAWN[1] + 4, SPAWN[2])],
              (SPAWN[0] + 28, SPAWN[1] + 4, SPAWN[2]))
        units.append((CREATURE_GUID(299, 2), 299, SPAWN[0] + 20, SPAWN[1], F_FORWARD | F_SPLINE, sp, {}))
        units.append((CREATURE_GUID(6, 3), 6, SPAWN[0] - 12, SPAWN[1] + 7, F_SWIMMING, None, {"pitch": 0.3}))   # Kobold
        units.append((CREATURE_GUID(6, 4), 6, SPAWN[0] - 14, SPAWN[1] - 3, F_JUMPING | F_FORWARD, None,
                      {"jump": (-7.95, 0.0, 1.0, 7.0), "fall": 120}))
        units.append((CREATURE_GUID(197, 5), 197, SPAWN[0] + 3, SPAWN[1] - 2, F_SPLINE_ELEV, None, {"elev": 1.5}))  # Marshal McBride
        units.append((CREATURE_GUID(80, 6), 80, SPAWN[0] + 30, SPAWN[1] - 9, F_TRANSPORT, None,
                      {"transport": (0x1FC0000000000123, 1.0, 2.0, 3.0, 0.5)}))
        sp2 = (0x40000, 1.25, 0, 3000, [(SPAWN[0] - 20, SPAWN[1] + 9, SPAWN[2])], (SPAWN[0] - 25, SPAWN[1] + 9, SPAWN[2]))
        units.append((CREATURE_GUID(299, 7), 299, SPAWN[0] - 20, SPAWN[1] + 4, F_SPLINE, sp2, {}))
        for g, entry, x, y, mf, spl, kw in units:
            uf = UF_HIGHGUID | (UF_MELEE if g & 0xFF == 1 else 0)
            blocks.append(create(g, 3, living_block(uf, mf, (x, y, SPAWN[2]), 1.0, spline=spl, **kw),
                                 unit_fields(g, entry, 2, 50 + (g & 0xFF), 600 + entry)))
        # a gameobject (HAS_POSITION, no LIVING) and a dynamic object
        go = 0xF110000000000000 | (1731 << 24) | 9
        blocks.append(create(go, 5, position_block(UF_HIGHGUID | UF_ALL, (SPAWN[0] + 6, SPAWN[1] + 6, SPAWN[2]), 0.3),
                             {0: 9, 1: 0xF1100000, 2: 0x21, 3: 1731, 4: 1.0, 8: 1310}))
        # a MOVEMENT-only block (full u64 guid) moving the first wolf
        blocks.append(movement_upd(CREATURE_GUID(299, 1), living_block(0, 0, (SPAWN[0] + 11, SPAWN[1] + 4, SPAWN[2]), 2.0)))
        self.send(0x0A9, update_packet(blocks))
        # the other player, uncompressed, then starts walking (relayed MSG_MOVE_*)
        of = unit_fields(self.other, 0, 7, 140, 50); of[2] = 0x19; of[193] = 0x01020304
        self.send(0x0A9, update_packet([create(self.other, 4, living_block(UF_ALL, 0, tuple(self.other_pos), 0.0), of)]))

        # 60 more wolves in one COMPRESSED update, plus VALUES (health) and OUT_OF_RANGE (2 of them)
        blocks = []
        for i in range(60):
            g = CREATURE_GUID(299, 100 + i)
            a = i / 60 * 2 * math.pi
            blocks.append(create(g, 3, living_block(UF_HIGHGUID, 0, (SPAWN[0] + 60 * math.cos(a), SPAWN[1] + 60 * math.sin(a), SPAWN[2]), a),
                                 unit_fields(g, 299, 2, 55, 903)))
        blocks.append(values(CREATURE_GUID(299, 100), {22: 0}))      # one dies
        blocks.append(out_of_range([CREATURE_GUID(299, 158), CREATURE_GUID(299, 159)]))
        raw = update_packet(blocks)
        self.send(0x1F6, struct.pack("<I", len(raw)) + zlib.compress(raw, 6))
        # monster moves: linear with packed points, uncompressed (flying), stop, facing angle, and compressed batch
        w = CREATURE_GUID(299, 101)
        s0 = (SPAWN[0] + 60 * math.cos(1 / 60 * 2 * math.pi), SPAWN[1] + 60 * math.sin(1 / 60 * 2 * math.pi), SPAWN[2])
        mids = [(s0[0] + 2, s0[1] + 1, s0[2]), (s0[0] + 4, s0[1] + 3, s0[2] + 0.5)]
        self.send(0x0DD, monster_move(w, s0, (s0[0] + 6, s0[1] + 6, s0[2] + 1), mids, 3000))
        w2 = CREATURE_GUID(299, 102)
        self.send(0x0DD, monster_move(w2, (0.0, 0.0, 0.0), None, None, 2000, flags=0x200,
                                      uncompressed_pts=[(SPAWN[0], SPAWN[1] + 50, SPAWN[2] + 10), (SPAWN[0], SPAWN[1] + 52, SPAWN[2] + 12)]))
        self.send(0x0DD, monster_move(CREATURE_GUID(299, 103), (SPAWN[0] + 1, SPAWN[1] + 1, SPAWN[2]), None, None, 0, kind=1))
        self.send(0x0DD, monster_move(CREATURE_GUID(299, 104), (SPAWN[0] + 2, SPAWN[1] + 2, SPAWN[2]),
                                      (SPAWN[0] + 3, SPAWN[1] + 2, SPAWN[2]), [], 500, kind=4, facing=2.5))
        inner = b""
        for k in (105, 106):
            mm = monster_move(CREATURE_GUID(299, k), (SPAWN[0] + k - 100, SPAWN[1], SPAWN[2]), (SPAWN[0] + k - 98, SPAWN[1], SPAWN[2]), [], 1000)
            inner += struct.pack("<BH", len(mm) + 2, 0x0DD) + mm
        self.send(0x2FB, struct.pack("<I", len(inner)) + zlib.compress(inner))
        self.send(0x0AA, struct.pack("<Q", CREATURE_GUID(299, 157)))   # destroy one
        # 1 self + 6 items + 7 units + 1 GO + 1 other + 60 - 2 out of range - 1 destroyed
        exp.update({"total": 1 + 6 + 7 + 1 + 1 + 60 - 3, "units": 7 + 57, "players": 2, "gameobjects": 1, "items": 6,
                    "dead": 1, "monster_moves": 6, "compressed_batches": 1})
        RESULTS5["sent_update_packets"] = 3
        # schedule the other player's walk and the forced-movement tests
        self.events = [(1.0, self.other_start), (1.5, self.other_heartbeat), (2.0, self.other_heartbeat),
                       (2.5, self.other_stop), (3.0, self.force_speed), (5.0, self.root), (5.6, self.unroot),
                       (6.5, self.teleport)]
        # --fight: one more creature, 11.7 yd away, is the victim (a name nothing else has)
        self.wolf = CREATURE_GUID(69, 200)
        self.wolf_pos = (SPAWN[0] + 11, SPAWN[1] + 4)
        self.wolf_hp = 51
        if FIGHT:
            self.send(0x0A9, update_packet([create(self.wolf, 3, living_block(UF_HIGHGUID, 0, (*self.wolf_pos, SPAWN[2]), 1.0),
                                                   unit_fields(self.wolf, 69, 3, self.wolf_hp, 669))]))
        self.attacking = None
        self.circled = False
        self.next_swing = self.next_bite = 0.0
        self.pos = (SPAWN[0], SPAWN[1], 3.07)    # client position/orientation from its movement packets
        if FIGHT:
            self.events = [(0.5, self.npc_say), (0.7, self.system_msg), (0.8, self.channel_joined), (0.3, self.send_spells)]
            RESULTS5["expected"]["fight"] = True

    # ---- chat (vMaNGOS ChatHandler::BuildChatPacket)
    def chat(self, ctype, lang, guid, text, name=None, channel=None):
        out = bytes([ctype]) + struct.pack("<I", lang)
        if ctype in (0x0D, 0x1A): out += struct.pack("<I", len(name) + 1) + name.encode() + b"\0" + struct.pack("<Q", 0)
        elif ctype in (0x00, 0x01, 0x05): out += struct.pack("<QQ", guid, guid)
        elif ctype in (0x0B, 0x0C): out += struct.pack("<QI", guid, len(name) + 1) + name.encode() + b"\0" + struct.pack("<Q", 0)
        elif ctype == 0x0E: out += channel.encode() + b"\0" + struct.pack("<IQ", 0, guid)
        else: out += struct.pack("<Q", guid)
        out += struct.pack("<I", len(text) + 1) + text.encode() + b"\0" + b"\0"
        self.send(0x096, out)
    def npc_say(self): self.chat(0x0B, 0, CREATURE_GUID(197, 5), "Stay alert, recruit.", name="Marshal McBride")
    def system_msg(self): self.chat(0x0A, 0, 0, "Welcome to the fake realm.")
    def channel_joined(self): self.send(0x099, b"\x02" + b"General\0" + struct.pack("<II", 0, 0))

    # ---- spells (mangos-zero Player::SendInitialSpells / SendInitialActionButtons, Spell::SendSpellStart/Go)
    SPELLS = (78, 6603, 2457, 668, 9998, 9999)
    def send_spells(self):
        b = struct.pack("<BH", 0, len(self.SPELLS)) + b"".join(struct.pack("<HH", s, i) for i, s in enumerate(self.SPELLS))
        b += struct.pack("<H", 1) + struct.pack("<HHHII", 2457, 0, 0, 0, 3000)      # Battle Stance not ready for 3 s
        self.send(0x12A, b)
        bar = [0] * 120
        bar[0], bar[1], bar[2], bar[3], bar[4] = 6603, 78, 9999, 2457, (0x40 << 24) | 5   # slot 4 is a macro
        bar[5] = 9998
        self.send(0x129, struct.pack("<120I", *bar))
        self.pending = []                                # (time, callable) for casts in progress
    def spell_packet(self, op, spell, ms=0, flags=0):
        # SMSG_SPELL_START / SMSG_SPELL_GO: packed item guid (none), packed caster, spell, flags, [castTime | hit list], targets
        out = packguid(0) + packguid(self.me) + struct.pack("<IH", spell, flags)
        if op == 0x131: out += struct.pack("<IH", ms, 0)                       # cast time, targets: self
        else: out += struct.pack("<BQBH", 1, self.wolf, 0, 2) + packguid(self.wolf)   # 1 hit, 0 misses, unit target
        self.send(op, out)
    def cast_packet(self, body):
        spell, mask = struct.unpack_from("<IH", body, 0)
        rest = body[6:]
        target = None
        if mask == 2:
            m = rest[0]; n = bin(m).count("1")
            if len(rest) != 1 + n: self.err(f"CAST_SPELL packed guid size {len(rest)}")
            g = 0
            idx = 1
            for i in range(8):
                if m & (1 << i): g |= rest[idx] << (8 * i); idx += 1
            target = g
        elif mask == 0:
            if rest: self.err("CAST_SPELL self cast with trailing bytes")
        else: self.err(f"CAST_SPELL unexpected target mask {mask:#x}")
        if RESULTS5["selection"] and mask != 2: self.err("CAST_SPELL without the selected target")
        if mask == 2 and target != RESULTS5["selection"]: self.err(f"CAST_SPELL target {target:x} != selection")
        RESULTS5["casts"].append([spell, mask])
        if spell not in self.SPELLS: self.err(f"cast of unknown spell {spell}"); self.send(0x130, struct.pack("<IBB", spell, 2, 6)); return
        if spell == 78:                                  # Heroic Strike: instant, damage log, cooldown
            self.spell_packet(0x132, spell)
            self.send(0x250, packguid(self.wolf) + packguid(self.me) + struct.pack("<IIBIIBBIIB", spell, 21, 0, 0, 0, 0, 0, 0, 2, 0))
            self.send(0x134, struct.pack("<QII", self.me, spell, 4000))
        elif spell == 2457:
            self.spell_packet(0x132, spell)
        elif spell == 9999:                              # 2 s cast
            self.spell_packet(0x131, spell, 2000)
            self.pending.append((self.now() + 2.0, lambda: self.spell_packet(0x132, spell)))
        elif spell == 9998:                              # 3 s cast, interrupted after 1 s: SMSG_SPELL_FAILURE (full guid)
            self.spell_packet(0x131, spell, 3000)
            self.pending.append((self.now() + 1.0, lambda: self.send(0x133, struct.pack("<QIB", self.me, spell, 0x23))))
        else:
            self.send(0x130, struct.pack("<IBB", spell, 2, 0x1F))     # 6603 "Attack" is never cast: if it arrives it is a bug
            self.err(f"spell {spell} should not reach the server as a cast")

    # ---- melee (vMaNGOS WorldPackets::Combat), driven from tick()
    def facing_ok(self):
        x, y, o = self.pos
        a = math.atan2(self.wolf_pos[1] - y, self.wolf_pos[0] - x)
        return abs((a - o + math.pi) % (2 * math.pi) - math.pi) < math.pi / 2
    def in_range(self): return math.dist(self.pos[:2], self.wolf_pos) <= 5.0
    def state_update(self, attacker, victim, dmg, hit_info=2, victim_state=1):
        self.send(0x14A, struct.pack("<I", hit_info) + packguid(attacker) + packguid(victim) + struct.pack("<iB", dmg, 1) +
                  struct.pack("<IfIII", 0, float(dmg), dmg, 0, 0) + struct.pack("<IIIi", victim_state, 0, 0, 0))
    def fight_tick(self, t):
        if not self.attacking or self.wolf_hp <= 0 or t < self.next_swing: return
        self.next_swing = t + 0.6
        if not self.in_range():
            RESULTS5["attack_errors"].append("range"); self.send(0x145, b""); return
        if not self.facing_ok():
            RESULTS5["attack_errors"].append("facing"); self.send(0x146, b""); return
        RESULTS5["swings"] += 1
        miss = RESULTS5["swings"] == 2                  # one miss on the way
        dmg = 0 if miss else 17
        self.wolf_hp = max(0, self.wolf_hp - dmg)
        if RESULTS5["swings"] == 2 and not self.circled:   # the wolf runs round behind us: we must turn
            self.circled = True
            x, y, o = self.pos
            dest = (x - 2.5 * math.cos(o), y - 2.5 * math.sin(o), SPAWN[2])
            self.send(0x0DD, monster_move(self.wolf, (*self.wolf_pos, SPAWN[2]), dest, [], 300))
            self.wolf_pos = dest[:2]
            self.next_swing = t + 0.9
        self.state_update(self.me, self.wolf, dmg, hit_info=0x10 if miss else 2)
        fields = {22: self.wolf_hp}
        if self.wolf_hp == 0: fields[143] = 1          # UNIT_DYNAMIC_FLAGS: lootable
        self.send(0x0A9, update_packet([values(self.wolf, fields)]))
        if self.wolf_hp == 0:
            RESULTS5["wolf_dead"] = True
            self.attacking = None
            self.send(0x144, packguid(self.me) + packguid(self.wolf) + struct.pack("<I", 0))
            self.send(0x1D0, struct.pack("<QIBIf", self.wolf, 45, 0, 45, 1.0))
        elif t >= self.next_bite:                        # the wolf bites back
            self.next_bite = t + 1.1
            self.my_hp -= 3
            self.state_update(self.wolf, self.me, 3)
            self.send(0x0A9, update_packet([values(self.me, {22: self.my_hp})]))

    # ---- other player (relayed movement)
    def other_move(self, op, flags):
        self.send(op, packguid(self.other) + movement_info(flags, int(self.now() * 1000), *self.other_pos, 1.5708))
    def other_start(self): self.other_move(0xB5, F_FORWARD)
    def other_heartbeat(self):
        self.other_pos[1] += 3.5
        self.other_move(0xEE, F_FORWARD)
    def other_stop(self):
        self.other_pos[1] += 3.5
        self.other_move(0xB7, 0)
        RESULTS5["expected"]["other_final"] = list(self.other_pos)

    # ---- forced movement
    def force_speed(self):
        self.counter += 1
        self.expect_ack[0xE3] = self.counter
        self.send(0x0E2, packguid(self.me) + struct.pack("<If", self.counter, 14.0))
    def root(self):
        self.counter += 1
        self.expect_ack[0xE9] = self.counter
        self.send(0x0E8, packguid(self.me) + struct.pack("<I", self.counter))
    def unroot(self):
        self.counter += 1
        self.expect_ack[0xEB] = self.counter
        self.send(0x0EA, packguid(self.me) + struct.pack("<I", self.counter))
    def teleport(self):
        self.counter += 1
        self.tele_pos = (SPAWN[0] + 40, SPAWN[1] - 40, SPAWN[2] + 2)
        self.expect_ack[0xC7] = self.counter
        self.send(0x0C7, packguid(self.me) + struct.pack("<I", self.counter) +
                  movement_info(0, int(self.now() * 1000), *self.tele_pos, 0.5))

    def tick(self):
        t = self.now()
        while self.events and self.events[0][0] <= t:
            self.events.pop(0)[1]()
        if FIGHT:
            self.fight_tick(t)
            due = [p for p in getattr(self, "pending", []) if p[0] <= t]
            for p in due:
                self.pending.remove(p); p[1]()

    # ---- client packets
    def err(self, msg):
        if len(RESULTS5["errors"]) < 30: RESULTS5["errors"].append(f"{self.now():.2f}s {msg}")
        print("[stage5] ERROR", msg)

    def parse_mi(self, body, off=0):
        flags, t, x, y, z, o = struct.unpack_from("<IIffff", body, off); off += 24
        if flags & F_TRANSPORT: raise ValueError("client sent transport data")
        if flags & F_SWIMMING: off += 4
        fall, = struct.unpack_from("<I", body, off); off += 4
        if flags & F_JUMPING: off += 16
        if flags & F_SPLINE_ELEV: off += 4
        return (flags, t, x, y, z, o, fall), off

    MOVE_OPS = {0xB5: "START_FORWARD", 0xB6: "START_BACKWARD", 0xB7: "STOP", 0xB8: "START_STRAFE_LEFT",
                0xB9: "START_STRAFE_RIGHT", 0xBA: "STOP_STRAFE", 0xBB: "JUMP", 0xC9: "FALL_LAND", 0xDA: "SET_FACING",
                0xEE: "HEARTBEAT", 0xC2: "SET_RUN_MODE", 0xC3: "SET_WALK_MODE"}

    def client_packet(self, op, body):
        if op == 0x26A:                                  # CMSG_SET_ACTIVE_MOVER: u64 guid (vMaNGOS needs it)
            g, = struct.unpack("<Q", body)
            if len(body) != 8 or g != self.me: self.err(f"SET_ACTIVE_MOVER {body.hex()}")
            RESULTS5["active_mover"] += 1
            return True
        if op in self.MOVE_OPS and not RESULTS5["active_mover"]:
            # vMaNGOS drops movement from a client that never named its mover ("fake client")
            RESULTS5["moves_before_mover"] += 1
            self.err(f"{self.MOVE_OPS[op]} before CMSG_SET_ACTIVE_MOVER: a vMaNGOS server ignores it")
            return True
        if op in self.MOVE_OPS:
            try:
                (flags, t, x, y, z, o, fall), end = self.parse_mi(body)
            except Exception as e:
                self.err(f"{self.MOVE_OPS[op]}: unparsable ({e})"); return True
            if end != len(body): self.err(f"{self.MOVE_OPS[op]}: {len(body) - end} trailing bytes")
            RESULTS5["move_packets"] += 1
            name = self.MOVE_OPS[op]
            if op == 0xEE:
                RESULTS5["heartbeats"] += 1
                if not flags & (F_FORWARD | F_BACKWARD | F_SL | F_SR): self.err("heartbeat while not moving")
            if op == 0xDA: RESULTS5["facings"] += 1
            if op == 0xC3:                               # stage 5.11: walk mode (stick pushed partway)
                RESULTS5["walk_modes"] = RESULTS5.get("walk_modes", 0) + 1
                if not flags & 0x100: self.err("SET_WALK_MODE without the walk flag")
            if op == 0xC2:
                RESULTS5["run_modes"] = RESULTS5.get("run_modes", 0) + 1
                if flags & 0x100: self.err("SET_RUN_MODE with the walk flag")
            if flags & 0x100 and flags & 0xF: RESULTS5["walk_moves"] = RESULTS5.get("walk_moves", 0) + 1
            if op in (0xB5, 0xB6, 0xB8, 0xB9): RESULTS5["starts"] += 1
            if op in (0xB7, 0xBA): RESULTS5["stops"] += 1
            want = {0xB5: F_FORWARD, 0xB6: F_BACKWARD, 0xB8: F_SL, 0xB9: F_SR}.get(op)
            if want and not flags & want: self.err(f"{name} without its flag (0x{flags:x})")
            if op == 0xB7 and flags & (F_FORWARD | F_BACKWARD): self.err("STOP with forward/backward flag set")
            if op == 0xBA and flags & (F_SL | F_SR): self.err("STOP_STRAFE with strafe flag set")
            if self.rooted and flags & (F_FORWARD | F_BACKWARD | F_SL | F_SR): self.err(f"{name} while rooted")
            if not (0 <= o < 2 * math.pi + 1e-4): self.err(f"orientation {o} outside [0, 2pi)")
            if self.tele_pos and not RESULTS5["teleport_ack"]:
                pass                   # movement before the ACK is ignored by real servers
            elif self.tele_pos and not RESULTS5["after_teleport_ok"]:
                d = math.dist((x, y), self.tele_pos[:2])
                if d < 3: RESULTS5["after_teleport_ok"] = True
                else: self.err(f"first movement after teleport {d:.1f} yd from the destination")
            if self.last:
                lt, lx, ly = self.last
                dt = (t - lt) / 1000.0
                if dt < 0: self.err(f"time went backwards ({lt} -> {t})")
                elif dt > 0.05:
                    v = math.dist((x, y), (lx, ly)) / dt
                    RESULTS5["max_speed_seen"] = max(RESULTS5["max_speed_seen"], v)
                    # walk mode on land: walk speed, also backwards (vMaNGOS GetMoveTypeForMovementInfo)
                    lim = 2.5 if (self.last_flags & 0x100) and not (self.last_flags & F_SWIMMING) else self.speed
                    if v > lim * 1.15 + 0.5: self.err(f"{name}: speed {v:.2f} yd/s > {lim}")
            self.last = (t, x, y)
            self.last_flags = flags
            self.pos = (x, y, o)
            return True
        if op in (0xE3, 0xE9, 0xEB, 0xC7):
            if op not in self.expect_ack: self.err(f"unexpected ACK 0x{op:X}"); return True
            g, cnt = struct.unpack_from("<QI", body, 0)
            if g != self.me: self.err(f"ACK 0x{op:X} with guid {g:x}")
            if cnt != self.expect_ack.pop(op): self.err(f"ACK 0x{op:X} counter {cnt}")
            if op == 0xC7:
                if len(body) != 16: self.err(f"TELEPORT_ACK size {len(body)} != 16")
                RESULTS5["teleport_ack"] = True
                self.last = (struct.unpack_from("<I", body, 12)[0], *self.tele_pos[:2])
                return True
            (flags, t, x, y, z, o, fall), end = self.parse_mi(body, 12)
            if op == 0xE3:
                if len(body) != end + 4: self.err(f"speed ACK size {len(body)} != {end + 4}")
                sp, = struct.unpack_from("<f", body, end)
                if sp != 14.0: self.err(f"speed ACK echoes {sp}")
                RESULTS5["speed_ack"] = True
                self.speed = 14.0
            else:
                if len(body) != end: self.err(f"root ACK size {len(body)} != {end}")
                if op == 0xE9:
                    RESULTS5["root_ack"] = True; self.rooted = True
                    if not flags & F_ROOT: self.err("root ACK without the ROOT flag")
                else:
                    RESULTS5["unroot_ack"] = True; self.rooted = False
            return True
        if FIGHT and op == 0x12E:
            self.cast_packet(body); return True
        if FIGHT and op == 0x12F:
            RESULTS5["cancels"] += 1; return True
        if FIGHT and op in (0x095, 0x13D, 0x141, 0x142, 0x15D, 0x15E, 0x108, 0x15F, 0x097):
            return self.fight_packet(op, body)
        if op == 0x050:
            g, = struct.unpack("<Q", body)
            RESULTS5["name_queries"] += 1
            name = {self.other: "Otherguy"}.get(g, "Unknown")
            self.send(0x051, struct.pack("<Q", g) + name.encode() + b"\0\0" + struct.pack("<III", 1, 0, 1))
            return True
        if op == 0x060:
            entry, g = struct.unpack("<IQ", body)
            RESULTS5["creature_queries"] += 1
            names = {299: ("Young Wolf", ""), 69: ("Diseased Timber Wolf", ""), 6: ("Kobold Vermin", ""), 197: ("Marshal McBride", "Northshire Guard"),
                     80: ("Kobold Laborer", ""), 6491: ("Spirit Healer", "")}
            n, sub = names.get(entry, ("?", ""))
            body = struct.pack("<I", entry) + n.encode() + b"\0\0\0\0" + sub.encode() + b"\0" + struct.pack("<IIIIIII", 0, 1, 0, 0, 0, 0, 600 + entry) + b"\0\0"
            self.send(0x061, body)
            return True
        if op == 0x05E:
            entry, g = struct.unpack("<IQ", body)
            RESULTS5["go_queries"] += 1
            self.send(0x05F, struct.pack("<III", entry, 3, 1310) + b"Mailbox\0\0\0\0" + b"\0" * 24)
            return True
        return False

    def fight_packet(self, op, body):
        if op == 0x095:                                  # CMSG_MESSAGECHAT: u32 type, u32 lang, [target], text
            ctype, lang = struct.unpack_from("<II", body, 0)
            parts = body[8:].split(b"\0")
            to = None
            if ctype in (6, 14): to, text = parts[0].decode(), parts[1].decode()
            else: text = parts[0].decode()
            if len(body) != 8 + sum(len(p) + 1 for p in parts[:2 if to is not None else 1]): self.err("MESSAGECHAT size")
            RESULTS5["chat_in"].append([ctype, lang, to, text]); RESULTS5["lang"] = lang
            if lang != 7: self.err(f"chat language {lang}, a human speaks Common (7)")
            if ctype == 6:
                if to != "Otherguy": self.send(0x2A9, to.encode() + b"\0"); return True   # CHAT_PLAYER_NOT_FOUND
                self.chat(0x07, lang, self.other, text)            # WHISPER_INFORM
                self.chat(0x06, lang, self.other, "hi " + text)    # the answer
            elif ctype == 14: self.chat(0x0E, lang, self.me, text, channel=to)
            else: self.chat(ctype, lang, self.me, text)
            return True
        if op == 0x097:                                  # CMSG_JOIN_CHANNEL: name, password
            name = body.split(b"\0")[0]
            self.send(0x099, b"\x02" + name + b"\0" + struct.pack("<II", 0, 0)); return True
        if op == 0x13D:
            RESULTS5["selection"] = struct.unpack("<Q", body)[0]; return True
        if op == 0x141:                                  # CMSG_ATTACKSWING
            g, = struct.unpack("<Q", body)
            if len(body) != 8: self.err("ATTACKSWING size")
            if g != self.wolf: self.err(f"attack on {g:x}, expected the wolf"); self.send(0x149, b""); return True
            if RESULTS5["selection"] != g: self.err("ATTACKSWING without selecting the target first")
            if self.wolf_hp <= 0: self.send(0x148, b""); return True
            self.attacking = g
            self.send(0x143, struct.pack("<QQ", self.me, g))   # SMSG_ATTACKSTART: full guids
            return True
        if op == 0x142:
            RESULTS5["attack_stops"] += 1
            if self.attacking: self.send(0x144, packguid(self.me) + packguid(self.attacking) + struct.pack("<I", 0))
            self.attacking = None
            return True
        if op == 0x15D:                                  # CMSG_LOOT
            g, = struct.unpack("<Q", body)
            if g != self.wolf or self.wolf_hp > 0 or math.dist(self.pos[:2], self.wolf_pos) > 5.0:
                self.err("loot request for a living/far/other target")
                self.send(0x160, struct.pack("<QBB", g, 0, 4)); return True
            self.send(0x160, struct.pack("<QBIB", g, 1, 23, 1) + struct.pack("<BIIIIIB", 0, 7073, 1, 6614, 0, 0, 0))
            RESULTS5["loot"].append("open"); return True
        if op == 0x15E:
            RESULTS5["loot"].append("money"); self.send(0x165, b""); return True
        if op == 0x108:
            RESULTS5["loot"].append(f"item{body[0]}"); self.send(0x162, bytes([body[0]])); return True
        if op == 0x15F:
            g, = struct.unpack("<Q", body)
            RESULTS5["loot"].append("release"); self.send(0x161, struct.pack("<QB", g, 1)); return True
        return False

    def finish(self):
        if self.expect_ack: self.err(f"ACKs never received: {sorted(hex(k) for k in self.expect_ack)}")
