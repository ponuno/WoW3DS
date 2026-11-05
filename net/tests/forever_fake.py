"""forever_fake.py — a stand-in WoW Forever server for the probe's test (net/tests/run_forever.sh): the REST login (HTTPS), the
Battle.net RPC (TLS; the messages are encoded / decoded with the real protobuf definitions of a TrinityCore Forever fork, from
data/forever_bnet.desc, so the probe's hand-written protobuf is checked against an independent encoder) and the world server
(greetings, AUTH_CHALLENGE, AUTH_SESSION with its digest, the key derivation written again here from TrinityCore's WorldSocket,
ENTER_ENCRYPTED_MODE, AES-256-GCM both ways with the packet counters, AUTH_RESPONSE, ENUM_CHARACTERS_RESULT, two compressed
packets sharing one deflate stream, PING/PONG). Every check that fails is printed as "FAKE FAIL ..." and makes the exit code 1.
  forever_fake.py <cert.pem> <key.pem> <rest port> <bnet port> <world port> <build>"""
import hashlib, hmac, json, os, socket, ssl, struct, sys, threading, time, zlib
from http.server import BaseHTTPRequestHandler, HTTPServer
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

CERT, KEY = sys.argv[1], sys.argv[2]
REST, BNET, WORLD, BUILD = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
MODE = sys.argv[7] if len(sys.argv) > 7 else ''   # 'create': an account with no character; the probe creates one and enters the world
EMAIL, PASSWORD, TICKET = 'TEST@EXAMPLE.ORG', 'secret pass', 'TC-0123456789ABCDEF'
GAME_ACCOUNT = 7
REALM_ADDR = (1 << 24) | (1 << 16) | 2   # 1-1-2
fails = []
def check(cond, what):
    if not cond:
        fails.append(what); print('FAKE FAIL', what, flush=True)
    return cond

# ---------------------------------------------------------------- protobuf classes from the fork's definitions
fds = descriptor_pb2.FileDescriptorSet()
fds.ParseFromString(open(os.path.join(os.path.dirname(__file__), 'data', 'forever_bnet.desc'), 'rb').read())
pool = descriptor_pool.DescriptorPool()
pool.AddSerializedFile(descriptor_pb2.DESCRIPTOR.serialized_pb)   # (the bgs option extensions extend it)
pending = list(fds.file)
while pending:
    left = []
    for fd in pending:
        try: pool.Add(fd)
        except Exception: left.append(fd)
    if len(left) == len(pending): raise SystemExit('descriptor set does not resolve')
    pending = left
def M(name): return message_factory.GetMessageClass(pool.FindMessageTypeByName(name))
Header, ConnectRequest, ConnectResponse = M('bgs.protocol.Header'), M('bgs.protocol.connection.v1.ConnectRequest'), M('bgs.protocol.connection.v1.ConnectResponse')
LogonRequest, LogonResult, NoData = M('bgs.protocol.authentication.v1.LogonRequest'), M('bgs.protocol.authentication.v1.LogonResult'), M('bgs.protocol.NoData')
ClientRequest, ClientResponse = M('bgs.protocol.game_utilities.v1.ClientRequest'), M('bgs.protocol.game_utilities.v1.ClientResponse')
GetAllReq, GetAllResp = M('bgs.protocol.game_utilities.v1.GetAllValuesForAttributeRequest'), M('bgs.protocol.game_utilities.v1.GetAllValuesForAttributeResponse')

def pack_json(prefix, obj):
    raw = (prefix + ':' + json.dumps(obj)).encode() + b'\0'
    return struct.pack('<I', len(raw)) + zlib.compress(raw)

# ---------------------------------------------------------------- REST
class Rest(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        check(self.path == '/bnetserver/login/', 'REST path ' + self.path)
        form = json.loads(body)
        vals = {i['input_id']: i['value'] for i in form.get('inputs', [])}
        check(form.get('program_id') == 'WoW' and form.get('platform_id') == 'Wn64', 'REST form ids %r' % form)
        ok = vals.get('account_name', '').upper() == EMAIL and vals.get('password') == PASSWORD
        check(ok, 'REST credentials %r' % vals)
        out = {'authentication_state': 'DONE', 'login_ticket': TICKET} if ok else {'authentication_state': 'DONE'}
        data = json.dumps(out).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json;charset=utf-8')
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
def rest_server():
    srv = HTTPServer(('127.0.0.1', REST), Rest)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(CERT, KEY)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()

# ---------------------------------------------------------------- Battle.net RPC
state = {'clientSecret': None, 'joinSecret': os.urandom(32)}
def recv_exact(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b
def send_frame(s, hdr, msg):
    hb = hdr.SerializeToString()
    s.sendall(struct.pack('>H', len(hb)) + hb + msg.SerializeToString())
def variant_value(v):
    for f in ('string_value', 'blob_value', 'uint_value', 'int_value', 'bool_value'):
        if v.HasField(f): return getattr(v, f)
    return None
def bnet_conn(s):
    while True:
        try: ln = struct.unpack('>H', recv_exact(s, 2))[0]
        except EOFError: return
        h = Header(); h.ParseFromString(recv_exact(s, ln))   # (raises on a header without its required fields)
        payload = recv_exact(s, h.size) if h.HasField('size') else b''
        resp = Header(service_id=0xFE, token=h.token, size=0)
        if h.service_hash == 0x65446991 and h.method_id == 1:
            r = ConnectRequest(); r.ParseFromString(payload)
            check(r.use_bindless_rpc, 'Connect without bindless RPC')
            out = ConnectResponse(); out.server_id.label = 1; out.server_id.epoch = 2; out.use_bindless_rpc = True
            resp.size = out.ByteSize(); send_frame(s, resp, out)
        elif h.service_hash == 0x0DECFC01 and h.method_id == 1:
            r = LogonRequest(); r.ParseFromString(payload)
            check(r.program == 'WoW' and r.platform == 'Wn64' and r.locale == 'enUS', 'Logon ids %s %s %s' % (r.program, r.platform, r.locale))
            check(r.application_version == BUILD, 'Logon build %d' % r.application_version)
            if not r.HasField('cached_web_credentials'):   # the client asks where to log in: the web login page
                send_frame(s, resp, NoData())
                ChallengeReq = M('bgs.protocol.challenge.v1.ChallengeExternalRequest')
                ch = ChallengeReq(payload_type='web_auth_url', payload=('https://127.0.0.1:%d/bnetserver/login/' % REST).encode())
                send_frame(s, Header(service_id=0, method_id=3, token=1, size=ch.ByteSize(), service_hash=0xBBDA171F), ch)
                state['asked'] = True
                continue
            check(r.cached_web_credentials == TICKET.encode(), 'Logon ticket %r' % r.cached_web_credentials)
            send_frame(s, resp, NoData())
            res = LogonResult(error_code=0); res.account_id.high = 0x100000000000000; res.account_id.low = 3
            g = res.game_account_id.add(); g.high = 0x200000200576F57; g.low = GAME_ACCOUNT
            res.session_key = os.urandom(64)
            send_frame(s, Header(service_id=0, method_id=5, token=1, size=res.ByteSize(), service_hash=0x71240E35), res)
        elif h.service_hash == 0x3FC1274D and h.method_id == 10:
            r = GetAllReq(); r.ParseFromString(payload)
            check(r.attribute_key.startswith('Command_RealmListRequest_v1'), 'GetAllValues key ' + r.attribute_key)
            out = GetAllResp(); out.attribute_value.add().string_value = '1-1-0'
            resp.size = out.ByteSize(); send_frame(s, resp, out)
        elif h.service_hash == 0x3FC1274D and h.method_id == 1:
            r = ClientRequest(); r.ParseFromString(payload)
            check(r.program == 0x576F57, 'ClientRequest program %x' % r.program)
            params = {a.name: variant_value(a.value) for a in r.attribute}
            cmd = [k for k in params if k.startswith('Command_')]
            out = ClientResponse()
            def put(name, **kw):
                a = out.attribute.add(); a.name = name
                for k, v in kw.items(): setattr(a.value, k, v)
            name = cmd[0].rsplit('_', 1)[0] if cmd else ''
            if name == 'Command_RealmListTicketRequest_v1':
                ident = params.get('Param_Identity', b'').decode()
                info = params.get('Param_ClientInfo', b'').decode()
                check(ident.startswith('JSONRealmListTicketIdentity:') and json.loads(ident.split(':', 1)[1])['gameAccountID'] == GAME_ACCOUNT, 'identity ' + ident)
                check(info.startswith('JSONRealmListTicketClientInformation:'), 'client info prefix')
                ci = json.loads(info.split(':', 1)[1])['info']
                for req in ('platform', 'buildVariant', 'type', 'timeZone', 'currentTime', 'textLocale', 'audioLocale', 'versionDataBuild', 'version',
                            'secret', 'clientArch', 'systemVersion', 'platformType', 'systemArch'):
                    check(req in ci, 'client info lacks ' + req)
                check(ci['version']['versionBuild'] == BUILD and ci['platform'] == 0x576E3634, 'client info build / platform')
                check(len(ci['secret']) == 32 and all(0 <= x < 256 for x in ci['secret']), 'client secret')
                state['clientSecret'] = bytes(ci['secret'])
                put('Param_RealmListTicket', blob_value=b'AuthRealmListTicket\0')
            elif name == 'Command_RealmListRequest_v1':
                check(params[cmd[0]] == '1-1-0', 'realm list sub-region %r' % params[cmd[0]])
                check(params.get('Param_RealmListTicket') == b'AuthRealmListTicket\0', 'realm list ticket')
                put('Param_RealmList', blob_value=pack_json('JSONRealmListUpdates', {'updates': [
                    {'update': {'wowRealmAddress': REALM_ADDR + 1, 'name': 'Old Realm', 'version': {'versionMajor': 1, 'versionMinor': 60, 'versionRevision': 1, 'versionBuild': 70450}, 'flags': 0, 'populationState': 1, 'cfgCategoriesID': 1}, 'deleting': False},
                    {'update': {'wowRealmAddress': REALM_ADDR, 'name': 'Forever Test', 'version': {'versionMajor': 1, 'versionMinor': 60, 'versionRevision': 1, 'versionBuild': BUILD}, 'flags': 0, 'populationState': 1, 'cfgCategoriesID': 1}, 'deleting': False}]}))
                put('Param_CharacterCountList', blob_value=pack_json('JSONRealmCharacterCountList', {'counts': [{'wowRealmAddress': REALM_ADDR, 'count': 1}]}))
            elif name == 'Command_RealmJoinRequest_v1':
                check(params.get('Param_RealmAddress') == REALM_ADDR, 'join address %r (the realm of our build)' % params.get('Param_RealmAddress'))
                put('Param_RealmJoinTicket', blob_value=json.dumps({'gameAccount': '7#1', 'platform': 0x576E3634, 'type': 0x576F57, 'clientArch': 0x783634}).encode())
                put('Param_ServerAddresses', blob_value=pack_json('JSONRealmListServerIPAddresses', {'families': [{'family': 1, 'addresses': [{'ip': '127.0.0.1', 'port': WORLD}]}]}))
                put('Param_JoinSecret', blob_value=state['joinSecret'])
            else:
                check(False, 'unknown client request %r' % cmd)
            resp.size = out.ByteSize(); send_frame(s, resp, out)
        else:
            check(False, 'unexpected RPC %08x.%d' % (h.service_hash, h.method_id))
def bnet_server():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(CERT, KEY)
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(('127.0.0.1', BNET)); ls.listen(4)
    while True:
        c, _ = ls.accept()
        threading.Thread(target=bnet_conn, args=(ctx.wrap_socket(c, server_side=True),), daemon=True).start()

# ---------------------------------------------------------------- world (written again from TrinityCore WorldSocket.cpp)
AUTH_CHECK_SEED = bytes.fromhex('DE3A2A8E6B895266889D7E7A771D5D1F4ED90C239BCD0EDCD2E8043A6864C7B0')
SESSION_KEY_SEED = bytes.fromhex('E81E8B5927621EAA861518EAC0BF668C6DBF8393BCAA80525B1EDC23A012B750')
ENCRYPTION_KEY_SEED = bytes.fromhex('71C9ED5AA70E4DFF4C36A65A3E468A4A5DA148C830474ADEF60D6CBE6FE45573')
def sha512(*p): return hashlib.sha512(b''.join(p)).digest()
def hmac512(k, *p): return hmac.new(k, b''.join(p), hashlib.sha512).digest()
def session_key(seed):
    o1, o2 = sha512(seed[:32]), sha512(seed[32:]); o0 = bytes(64); out = b''
    while len(out) < 40:
        o0 = sha512(o1, o0, o2); out += o0
    return out[:40]
class World:
    def __init__(self, s): self.s, self.rc, self.sc, self.key, self.z = s, 0, 0, None, zlib.compressobj(6, zlib.DEFLATED, -15)
    def send(self, opcode, payload, compress=False):
        data = struct.pack('<I', opcode) + payload
        if compress:
            uadler = zlib.adler32(data, 0x9827D8F1) & 0xFFFFFFFF
            comp = self.z.compress(data) + self.z.flush(zlib.Z_SYNC_FLUSH)
            data = struct.pack('<I', 0x4E000E) + struct.pack('<III', len(data), uadler, zlib.adler32(comp, 0x9827D8F1) & 0xFFFFFFFF) + comp
        tag = bytes(12)
        if self.key:
            iv = struct.pack('<QI', self.sc, 0x52565253)
            ct = AESGCM(self.key).encrypt(iv, data, None)
            data, tag = ct[:-16], ct[-16:-4]   # (the 12-byte tag is the full tag cut short)
        self.sc += 1
        self.s.sendall(struct.pack('<I', len(data)) + tag + data)
    def recv(self):
        hdr = recv_exact(self.s, 16)
        size = struct.unpack('<I', hdr[:4])[0]
        data = recv_exact(self.s, size)
        if self.key:
            iv = struct.pack('<QI', self.rc, 0x544E4C43)
            data = self._decrypt12(iv, data, hdr[4:16])   # (a 12-byte tag: the low-level GCM mode checks it)
        self.rc += 1
        return struct.unpack('<I', data[:4])[0], data[4:]
    def _decrypt12(self, iv, ct, tag12):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        dec = Cipher(algorithms.AES(self.key), modes.GCM(iv, tag12, min_tag_length=12)).decryptor()
        return dec.update(ct) + dec.finalize()
CONTINUED_SESSION_SEED = bytes([0x56, 0x5C, 0x61, 0x9C, 0x48, 0x3A, 0x52, 0x1F, 0x61, 0x5D, 0x05, 0x49, 0xB2, 0x9A, 0x39, 0xBF,
                                0x4B, 0x97, 0xB0, 0x1B, 0xF9, 0x6C, 0xDE, 0xD6, 0x80, 0x1D, 0xAB, 0x26, 0x02, 0xA9, 0x9B, 0x9D])
INST_KEY = 0x1122334455667788
def instance_conn(s):
    w = World(s)
    s.sendall(b'WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT - V2\n')
    hello = b''
    while not hello.endswith(b'\n'): hello += recv_exact(s, 1)
    server_ch = os.urandom(32)
    w.send(0x4E0000, os.urandom(32) + server_ch + b'\x01')
    op, d = w.recv()
    check(op == 0x460003, 'AUTH_CONTINUED_SESSION opcode %06x' % op)
    check(len(d) == 8 + 32 + 24 + 8 + 4 + 4, 'AUTH_CONTINUED_SESSION length %d' % len(d))
    local, digest = d[8:40], d[40:64]
    key, native, key3 = struct.unpack('<QII', d[64:80])
    sk = state['sk']
    check(key == INST_KEY and native == 0x01010002 and key3 == 7, 'AUTH_CONTINUED_SESSION key / realm %r' % ((hex(key), hex(native), key3),))
    check(digest == hmac512(sk, struct.pack('<Q', INST_KEY), local, server_ch, CONTINUED_SESSION_SEED)[:24], 'AUTH_CONTINUED_SESSION digest')
    w.send(0x4E0004, struct.pack('<i', 0) + os.urandom(64) + b'\x80')
    op, d = w.recv()
    check(op == 0x460005, 'instance ENTER_ENCRYPTED_MODE_ACK %06x' % op)
    w.key = hmac512(sk, local, server_ch, ENCRYPTION_KEY_SEED)[:32]
    w.send(0x4E0006, b'')   # RESUME_COMMS
    op, d = w.recv()
    check(op == 0x46000B and len(d) == 4, 'QUEUED_MESSAGES_END %06x' % op)
    w.send(0x47002F, struct.pack('<i4fI', 0, -8949.95, -132.49, 83.53, 0.0, 0))   # LOGIN_VERIFY_WORLD arrives here
    state['entered'] = True
    try:
        while True: w.recv()
    except Exception: pass

NEW_GUID = bytes([0x2A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04, 0x0A])   # low bytes + a type in the top bytes (so the mask is not trivial)
def pack_guid(g):
    mask = sum(1 << i for i, b in enumerate(g) if b)
    return struct.pack('<H', mask) + bytes(b for b in g if b)
def create_and_enter(w):
    op, d = w.recv()
    check(op == 0x440070, 'CMSG_CREATE_CHARACTER after an empty list (%06x)' % op)
    # the server's CreateCharacter::Read, written again: 6 bit name length, 4 option bits, 2 unknown, 6 bit surname length
    bitsval = int.from_bytes(d[:3], 'big')
    nlen = (bitsval >> 18) & 0x3F
    opts = (bitsval >> 14) & 0xF
    surlen = (bitsval >> 0) & 0x3F
    race, cls, sex = d[3], d[4], d[5]
    ncust, minus1, season = struct.unpack('<IiI', d[6:18])
    name = d[18:18 + nlen].decode()
    check((nlen, opts, surlen) == (5, 0, 0) and name == 'Probe', 'CREATE_CHARACTER bits / name %r' % ((nlen, opts, surlen, name),))
    check((race, cls, sex, ncust, minus1, season) == (1, 1, 0, 0, -1, 0), 'CREATE_CHARACTER fields %r' % ((race, cls, sex, ncust, minus1, season),))
    check(len(d) == 18 + nlen, 'CREATE_CHARACTER length %d' % len(d))
    w.send(0x4701AC, struct.pack('<I', 24) + pack_guid(NEW_GUID))
    op, d = w.recv()
    check(op == 0x440014, 'ENUM_CHARACTERS again after the creation (%06x)' % op)
    w.send(0x470018, bytes([0b10000000, 0b00000000]) + struct.pack('<IIiIIII', 0, 0, 60, 0, 0, 0, 0))
    op, d = w.recv()
    check(op == 0x440016, 'CMSG_PLAYER_LOGIN after the creation (%06x)' % op)
    check(d[:len(pack_guid(NEW_GUID))] == pack_guid(NEW_GUID), 'PLAYER_LOGIN carries the new GUID')
    rest = d[len(pack_guid(NEW_GUID)):]
    check(len(rest) == 5 and struct.unpack('<f', rest[:4])[0] > 0, 'PLAYER_LOGIN far clip / bits %r' % rest)
    # SMSG_CONNECT_TO: 1 payload, serial 0, connection 1 (instance), key, native realm address, key3; IPv4 127.0.0.1, port, an empty token
    w.send(0x4E0008, struct.pack('<IIBQII', 1, 0, 1, INST_KEY, 0x01010002, 7) + bytes([1, 127, 0, 0, 1]) + struct.pack('<H', WORLD) + bytes(13))
    deadline = time.time() + 30
    pings = 0
    while time.time() < deadline:
        try: op, d = w.recv()
        except EOFError: break
        if op == 0x460006:
            pings += 1
            w.send(0x4E0009, d[:4])
        elif op == 0x460007:
            break
    state['pings'] = pings

def world_conn(s):
    w = World(s)
    s.sendall(b'WORLD OF WARCRAFT CONNECTION - SERVER TO CLIENT - V2\n')
    hello = b''
    while not hello.endswith(b'\n'): hello += recv_exact(s, 1)
    check(hello == b'WORLD OF WARCRAFT CONNECTION - CLIENT TO SERVER - V2\n', 'client greeting %r' % hello)
    server_ch = os.urandom(32)
    w.send(0x4E0000, os.urandom(32) + server_ch + b'\x01')
    op, d = w.recv()
    check(op == 0x460001, 'AUTH_SESSION opcode %06x' % op)
    dos, region, bg, realm = struct.unpack('<QIII', d[:20])
    local, digest = d[20:52], d[52:76]
    bits = d[76]
    tsize = struct.unpack('<I', d[77:81])[0]
    ticket = d[81:81 + tsize]
    check((region, bg, realm) == (1, 1, 2), 'AUTH_SESSION realm %r' % ((region, bg, realm),))
    check(bits == 0 and json.loads(ticket)['gameAccount'] == '7#1', 'AUTH_SESSION ticket / bits')
    key_data = state['clientSecret'] + state['joinSecret']
    expect = hmac512(sha512(key_data, bytes(16)), local, server_ch, AUTH_CHECK_SEED)[:24]
    check(digest == expect, 'AUTH_SESSION digest (zero build key)')
    sk = session_key(hmac512(sha512(key_data), server_ch, local, SESSION_KEY_SEED))
    state['sk'] = sk
    ek = hmac512(sk, local, server_ch, ENCRYPTION_KEY_SEED)[:32]
    w.send(0x4E0004, struct.pack('<i', 0) + os.urandom(64) + b'\x80')
    op, d = w.recv()
    check(op == 0x460005 and d == b'', 'ENTER_ENCRYPTED_MODE_ACK %06x' % op)
    w.key = ek
    w.send(0x470001, struct.pack('<I', 0) + b'\x00')   # AUTH_RESPONSE OK, no success / wait info
    op, d = w.recv()
    check(op == 0x440014, 'ENUM_CHARACTERS after AUTH_RESPONSE (%06x)' % op)
    # ENUM_CHARACTERS_RESULT in the retail layout: 10 bits, 7 counts, then one character (a made-up body with the name in it)
    if MODE == 'create':
        w.send(0x470018, bytes([0b10000000, 0b00000000]) + struct.pack('<IIiIIII', 0, 0, 60, 0, 0, 0, 0))
        create_and_enter(w)
        return
    body = bytes([0b10000000, 0b00000000]) + struct.pack('<IIiIIII', 1, 0, 60, 0, 0, 0, 0) + b'\x00' * 18 + b'Testchar' + b'\x00' * 40
    w.send(0x470018, body)
    w.send(0x470124, b'Europe/Berlin\0' * 100, compress=True)    # SET_TIME_ZONE_INFORMATION, compressed (> 1 KB)
    w.send(0x4701B6, b'\x11' * 1500 + b'Europe/Berlin\0' * 50, compress=True)   # a second one: the stream goes on
    deadline = time.time() + 30
    pings = 0
    while time.time() < deadline:
        try: op, d = w.recv()
        except EOFError: break
        if op == 0x460006:
            pings += 1
            w.send(0x4E0009, d[:4])
        elif op == 0x460007:
            break
    state['pings'] = pings
    state['worldDone'] = True
def world_server():
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); ls.bind(('127.0.0.1', WORLD)); ls.listen(4)
    c, _ = ls.accept()
    def second():   # the instance connection
        try:
            c2, _ = ls.accept()
            instance_conn(c2)
        except Exception as e:
            if MODE == 'create': check(False, 'instance: %r' % e)
    threading.Thread(target=second, daemon=True).start()
    try: world_conn(c)
    except Exception as e: check(False, 'world: %r' % e)
    state['worldDone'] = True

for t in (rest_server, bnet_server, world_server): threading.Thread(target=t, daemon=True).start()
print('FAKE READY', flush=True)
t0 = time.time()
while not state.get('worldDone') and time.time() - t0 < 90: time.sleep(0.2)
check(state.get('worldDone'), 'the world session never ended')
check(state.get('pings', 0) >= 1 or MODE == 'create', 'no ping in the stay')
if MODE == 'create': check(state.get('entered'), 'the probe never entered the world')
check(state.get('asked'), 'the probe did not ask for the login page')
print('FAKE: %d failures' % len(fails), flush=True)
sys.exit(1 if fails else 0)
