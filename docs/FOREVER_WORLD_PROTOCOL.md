# WoW Forever (Classic 1.60.1, build 70532): world protocol as verified against a live server

Checked on 2026-11-03 with `w3dfprobe` against a self-hosted forever_docker server (packet log of 330 packets, 28 `SMSG_UPDATE_OBJECT`,
all of them parse to the byte with `tools/forever/fupdate.py`).

## Login sequence
1. `CMSG_ENUM_CHARACTERS` -> `SMSG_ENUM_CHARACTERS_RESULT`; `CMSG_CREATE_CHARACTER` -> `SMSG_CREATE_CHAR` (code 24 = ok, GUID).
2. The server lets a character in only after it listed it: ask for the list again after creating.
3. `CMSG_PLAYER_LOGIN` (packed GUID, float far clip, 1 bit) -> `SMSG_CONNECT_TO` (u32 payload count, u32 serial, u8 connection, u64 key,
   u32 native realm address, u32 key3, then u8 address type 1 = IPv4, 4 bytes, u16 port, token).
4. Second TCP connection to that address: greeting, `SMSG_AUTH_CHALLENGE`, `CMSG_AUTH_CONTINUED_SESSION` (u64 0, 32-byte challenge, 24-byte digest
   = HMAC-SHA512(session key, key u64 | our challenge | server challenge | seed)[0..24), u64 key, u32 realm, u32 key3), `SMSG_ENTER_ENCRYPTED_MODE`,
   ACK, `SMSG_RESUME_COMMS` -> `CMSG_QUEUED_MESSAGES_END` (u32 ms).
5. The world arrives on the second connection: `SMSG_LOGIN_VERIFY_WORLD`, then `SMSG_UPDATE_OBJECT` (compressed), `SMSG_ON_MONSTER_MOVE`, ...
   The server also sends `SMSG_TIME_SYNC_REQUEST` (0x600000; answer with `CMSG_TIME_SYNC_RESPONSE`, not done by the probe yet).

## SMSG_UPDATE_OBJECT
Header: u16 map, u32 block count, bit "unk" (1), bit "has destroy list" (+ u16 + u32 + GUIDs), u32 data size; then blocks.
Block (create, update type 1/2): u8 type, packed GUID (u16 mask + bytes), u8 object type (1 item, 5 unit, 7 active player, 8 game object),
movement section (21 flag bits MSB-first, u32 pause times, then Movement / Stationary position / Rotation / GameObject / ActivePlayer parts),
u32 size of the rest, u8 field flags, fragment ids (retail + 1; 3 = CGObject data, 200+ are tags: 204 unit, 206 game object, 205 player) ending 0xFF,
then for fragment 3: u8 "indirect active" (1) and ObjectData (i32 entry, u32 dynamic flags, f32 scale) + UnitData / GameObjectData.
Values update (type 0): packed GUID, u32 size, data (not decoded yet).
Units move by `MovementUpdate` (position, 9 speeds, optional spline); static objects use `Stationary`.
UnitData up to the faction (create, field flags 0 = another player's view): display id, npc flags, ..., health (i64), power x10, max health (i64), level.
Verified on Northshire: wolves entry 69/299, rabbits 721, NPCs 823/925, transports (type 15) at (0,0,0) sent with `ServerTime`.

Tools: `tools/forever/fwl.py` (packet log reader), `fupdate.py` (block parser), `ffields.py` (prints entry / display / level / health / position).
