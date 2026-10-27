#!/usr/bin/env python3
"""Minimal vanilla 1.12.1 realmd + world server for testing the client.

Written independently from the client: the server side of SRP6 (verifier,
B, S = (A * v^u)^b) follows the MaNGOS realmd algorithm using Python's own
big integers, so a successful login proves the client's math, framing and
header cipher against a second implementation.

    python3 fake_server.py [--port 3724] [--salt-zero-top] [--stage5 results.json]

--life (with --stage5): the stage-5.10 scenario of life5.py (inventory, loot window, gossip, 3 deaths).
--trade (with --stage5): the stage-5.12..5.14 scenario of trade5.py (faction reactions, quests, a vendor).
--party (with --stage5): the stage-5.17 scenario of party5.py (invitation, members, stats, leader, kick, leave).
--stage5: after entering the world, send real update objects / monster moves /
another player (stage5.py) and validate the client's movement packets.
Account: TEST / PASSWORD. World server listens on port+1.
"""
import hashlib, json, math, os, socket, struct, sys, threading, time, zlib
from stage5 import World5, SPAWN, RESULTS5

N = int("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7", 16)
g = 7
ACCOUNT, PASSWORD = "TEST", "PASSWORD"
SESSION = {}          # account -> K (bytes)
RESULTS = {"auth_ok": False, "world_ok": False, "addons_ok": False, "pings": 0, "logout": False}

def H(*parts):
    h = hashlib.sha1()
    for p in parts: h.update(p)
    return h.digest()

def le(n, size=None):
    if size is None: size = max(1, (n.bit_length() + 7) // 8)   # natural length, like BN_bn2bin
    return n.to_bytes(size, "little")

def from_le(b): return int.from_bytes(b, "little")

def recv_exact(c, n):
    out = b""
    while len(out) < n:
        chunk = c.recv(n - len(out))
        if not chunk: raise ConnectionError("closed")
        out += chunk
    return out

# ------------------------------------------------------------------ realmd
def realmd(conn, world_port, salt_zero_top):
    hdr = recv_exact(conn, 4)                     # cmd, protocol, size(u16)
    cmd, proto, size = hdr[0], hdr[1], struct.unpack("<H", hdr[2:4])[0]
    body = recv_exact(conn, size)
    assert cmd == 0, "expected LOGON_CHALLENGE"
    gamename = body[0:4]; ver = body[4:7]; build = struct.unpack("<H", body[7:9])[0]
    acc_len = body[29]; account = body[30:30 + acc_len].decode()
    print(f"[realmd] challenge: protocol {proto} game {gamename!r} version {tuple(ver)} build {build} account {account}")
    assert gamename == b"WoW\x00" and build == 5875 and account == ACCOUNT

    salt = bytearray(os.urandom(32))
    salt[31] = 0 if salt_zero_top else (salt[31] | 1)
    salt = bytes(salt)
    x = from_le(H(salt, H(f"{ACCOUNT}:{PASSWORD}".encode())))
    v = pow(g, x, N)
    b = from_le(os.urandom(19))
    B = (3 * v + pow(g, b, N)) % N
    pkt = bytes([0, 0, 0]) + le(B, 32) + bytes([1, g, 32]) + le(N, 32) + salt + os.urandom(16) + bytes([0])
    conn.sendall(pkt)

    proof = recv_exact(conn, 75)
    assert proof[0] == 1
    A = from_le(proof[1:33]); M1 = proof[33:53]
    u = from_le(H(le(A), le(B)))
    S = pow(A * pow(v, u, N), b, N)
    Sb = le(S, 32)
    h1, h2 = H(Sb[0::2]), H(Sb[1::2])
    K = bytes(x for pair in zip(h1, h2) for x in pair)
    hNg = bytes(p ^ q for p, q in zip(H(le(N)), H(le(g))))
    M = H(hNg, H(ACCOUNT.encode()), le(from_le(salt)), le(A), le(B), K)
    if M != M1:
        print("[realmd] client proof MISMATCH"); conn.sendall(bytes([1, 5])); return
    print("[realmd] client proof OK")
    RESULTS["auth_ok"] = True
    SESSION[ACCOUNT] = K
    M2 = H(le(A), M1, K)
    conn.sendall(bytes([1, 0]) + M2 + struct.pack("<I", 0))

    req = recv_exact(conn, 5)
    assert req[0] == 0x10
    realms = b""
    for name, port, flags in (("Fake Realm", world_port, 0), ("Offline Realm", world_port, 2)):
        realms += struct.pack("<IB", 1, flags) + name.encode() + b"\0" + f"127.0.0.1:{port}".encode() + b"\0"
        realms += struct.pack("<fBBB", 1.5, 1, 1, 1)
    body = struct.pack("<IB", 0, 2) + realms + struct.pack("<H", 2)   # vanilla footer
    conn.sendall(bytes([0x10]) + struct.pack("<H", len(body)) + body)
    print("[realmd] realm list sent")

# ------------------------------------------------------------------ world
class Crypt:
    def __init__(self, key): self.k = key; self.si = self.sp = self.ri = self.rp = 0
    def enc(self, data):     # server -> client headers
        out = bytearray()
        for c in data:
            x = ((c ^ self.k[self.si]) + self.sp) & 0xFF
            self.si = (self.si + 1) % len(self.k); self.sp = x; out.append(x)
        return bytes(out)
    def dec(self, data):     # client -> server headers
        out = bytearray()
        for c in data:
            x = ((c - self.rp) & 0xFF) ^ self.k[self.ri]
            self.ri = (self.ri + 1) % len(self.k); self.rp = c; out.append(x)
        return bytes(out)

def world(conn):
    crypt = None
    batch = []                                    # when not None-empty-list-active, packets are coalesced
    batching = {"on": False}
    def send(op, payload, split=False):
        hdr = struct.pack(">H", len(payload) + 2) + struct.pack("<H", op)
        if crypt: hdr = crypt.enc(hdr)
        data = hdr + payload
        if batching["on"]:
            batch.append(data); return
        if split:                                 # exercise partial reads in the client
            conn.sendall(data[:3]); time.sleep(0.05); conn.sendall(data[3:])
        else:
            conn.sendall(data)
    rx = {"buf": b"", "hdr": None}
    def recv():                                   # buffered: safe with a socket timeout
        while True:
            if rx["hdr"] is None and len(rx["buf"]) >= 6:
                hdr = rx["buf"][:6]; rx["buf"] = rx["buf"][6:]
                rx["hdr"] = crypt.dec(hdr) if crypt else hdr
            if rx["hdr"] is not None:
                size = struct.unpack(">H", rx["hdr"][0:2])[0]; op = struct.unpack("<I", rx["hdr"][2:6])[0]
                if len(rx["buf"]) >= size - 4:
                    body = rx["buf"][:size - 4]; rx["buf"] = rx["buf"][size - 4:]; rx["hdr"] = None
                    return op, body
            chunk = conn.recv(65536)
            if not chunk: raise ConnectionError("closed")
            rx["buf"] += chunk

    seed = struct.unpack("<I", os.urandom(4))[0]
    send(0x1EC, struct.pack("<I", seed))
    op, body = recv()
    assert op == 0x1ED, hex(op)
    build, _srv = struct.unpack("<II", body[0:8])
    end = body.index(b"\0", 8); account = body[8:end].decode()
    client_seed = struct.unpack("<I", body[end + 1:end + 5])[0]
    digest = body[end + 5:end + 25]
    addon_size = struct.unpack("<I", body[end + 25:end + 29])[0]
    addons = zlib.decompress(body[end + 29:])
    RESULTS["addons_ok"] = len(addons) == addon_size and b"Blizzard_AuctionUI" in addons
    K = SESSION.get(account)
    expect = H(account.encode(), b"\0\0\0\0", struct.pack("<I", client_seed), struct.pack("<I", seed), K)
    if digest != expect:
        print("[world] AUTH_SESSION digest MISMATCH"); send(0x1EE, bytes([0x0D])); return
    print(f"[world] AUTH_SESSION OK (build {build}, account {account}, addon block {addon_size} bytes inflated OK={RESULTS['addons_ok']})")
    crypt = Crypt(K)
    send(0x1EE, bytes([0x0C]) + struct.pack("<IBI", 0, 0, 0))

    state = {}
    conn.settimeout(0.05)
    while True:
        if state.get("w5"): state["w5"].tick()
        try:
            op, body = recv()
        except socket.timeout:
            continue
        except ConnectionError:
            print("[world] client disconnected"); return
        if op == 0x037:                            # CMSG_CHAR_ENUM
            chars = b""
            for guid, name, race, cls, lvl, x, y, z in ((1, "Testone", 1, 1, 12, -8949.95, -132.49, 83.53),
                                                       (2, "Testtwo", 4, 11, 60, 9947.52, 2482.73, 1316.21)):
                chars += struct.pack("<Q", guid) + name.encode() + b"\0" + bytes([race, cls, 0]) + b"\0" * 5
                chars += bytes([lvl]) + struct.pack("<IIfffII", 12, 0, x, y, z, 0, 0) + b"\0" + b"\0" * 12 + b"\0" * 100
            send(0x03B, bytes([2]) + chars, split=True)
            print("[world] character list sent")
        elif op == 0x03D:                          # CMSG_PLAYER_LOGIN
            guid = struct.unpack("<Q", body)[0]
            batching["on"] = True                  # like real servers: VERIFY_WORLD + first updates in one TCP write
            send(0x236, struct.pack("<Iffff", 0, *SPAWN, 3.07))   # SMSG_LOGIN_VERIFY_WORLD
            send(0x209, b"\0" * 128)               # SMSG_ACCOUNT_DATA_TIMES
            RESULTS["world_ok"] = True
            print(f"[world] player {guid} entered the world")
            if STAGE5:
                if "--life" in sys.argv:              # stage 5.10: items, gossip, death and resurrection
                    from life5 import WorldLife
                    w5 = WorldLife(send, guid)
                elif "--trade" in sys.argv:           # stages 5.12-5.14: factions, quests, a vendor
                    from trade5 import WorldTrade
                    w5 = WorldTrade(send, guid)
                elif "--party" in sys.argv:           # stage 5.17: a party
                    from party5 import WorldParty
                    w5 = WorldParty(send, guid)
                else:
                    w5 = World5(send, guid)
                state["w5"] = w5; w5.enter()
            else:
                send(0x0A9, os.urandom(3000))      # fake update object, bigger than one TCP read
            batching["on"] = False
            conn.sendall(b"".join(batch)); batch.clear()
        elif op == 0x1DC:                          # CMSG_PING
            seq, lat = struct.unpack("<II", body)
            RESULTS["pings"] += 1
            send(0x1DD, struct.pack("<I", seq))
            print(f"[world] ping {seq} (client latency {lat} ms)")
        elif op == 0x04B:                          # CMSG_LOGOUT_REQUEST
            send(0x04C, struct.pack("<IB", 0, 0)); time.sleep(0.2); send(0x04D, b"")
            RESULTS["logout"] = True
            print("[world] logout complete")
            if state.get("w5"):
                state["w5"].finish()
                out = sys.argv[sys.argv.index("--stage5") + 1]
                json.dump({**RESULTS, **RESULTS5}, open(out, "w"), indent=1)
                print(f"[world] stage-5 results written to {out}")
        elif state.get("w5") and state["w5"].client_packet(op, body):
            pass
        else:
            print(f"[world] unhandled opcode 0x{op:03X}")

def serve(port, handler, *args):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port)); s.listen(4)
    def loop():
        while True:
            c, _ = s.accept()
            threading.Thread(target=lambda: _wrap(handler, c, *args), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()

def _wrap(handler, c, *args):
    try: handler(c, *args)
    except Exception as e: print(f"[{handler.__name__}] error: {e!r}")
    finally: c.close()

STAGE5 = "--stage5" in sys.argv

if __name__ == "__main__":
    port = int(sys.argv[sys.argv.index("--port") + 1]) if "--port" in sys.argv else 3724
    serve(port, realmd, port + 1, "--salt-zero-top" in sys.argv)
    serve(port + 1, world)
    print(f"fake server: realmd {port}, world {port + 1}; account {ACCOUNT}/{PASSWORD}", flush=True)
    try:
        while True: time.sleep(0.5)
    except KeyboardInterrupt:
        print(RESULTS)
