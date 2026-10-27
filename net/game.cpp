#include "game.hpp"

#include <cstring>

namespace w3n {

namespace {
enum : uint16_t {
    SMSG_NEW_WORLD = 0x03E, SMSG_TRANSFER_PENDING = 0x03F,
    SMSG_FORCE_RUN_SPEED_CHANGE = 0x0E2, CMSG_FORCE_RUN_SPEED_CHANGE_ACK = 0x0E3,
    SMSG_FORCE_RUN_BACK_SPEED_CHANGE = 0x0E4, CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK = 0x0E5,
    SMSG_FORCE_SWIM_SPEED_CHANGE = 0x0E6, CMSG_FORCE_SWIM_SPEED_CHANGE_ACK = 0x0E7,
    SMSG_FORCE_MOVE_ROOT = 0x0E8, CMSG_FORCE_MOVE_ROOT_ACK = 0x0E9,
    SMSG_FORCE_MOVE_UNROOT = 0x0EA, CMSG_FORCE_MOVE_UNROOT_ACK = 0x0EB,
    SMSG_FORCE_WALK_SPEED_CHANGE = 0x2DA, CMSG_FORCE_WALK_SPEED_CHANGE_ACK = 0x2DB,
    SMSG_FORCE_SWIM_BACK_SPEED_CHANGE = 0x2DC, CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK = 0x2DD,
    // The client names the unit it moves. vMaNGOS-family cores ignore every movement packet until it
    // arrives (Player::GetConfirmedMover: "no mover client side, is this a fake client?"); the real
    // client sends it on entering the world and after every map change (WoWee does the same).
    CMSG_SET_ACTIVE_MOVER = 0x26A,
};

// Forced-movement packets start with the player's GUID, packed on vMaNGOS/CMaNGOS
// and a full u64 in some cores; the packet size tells which (rest = 'tail' bytes).
uint64_t readGuidBySize(Reader& r, size_t tail) {
    if (r.left() == 8 + tail) return r.u64();
    return r.packedGuid();
}
}  // namespace

void GameSession::attach(WorldClient& wc, uint32_t nowMs) {
    wc_ = &wc;
    self_ = wc.playerGuid();
    selfName.clear();
    for (const auto& c : wc.characters())
        if (c.guid == self_) selfName = c.name;
    map_ = wc.mapId();
    now_ = nowMs;
    objects.clear();
    objects.setSelf(self_);
    askedNames_.clear();
    askedCreatures_.clear();
    askedGos_.clear();
    chat.clear();
    hits.clear();
    target_ = 0;
    attackWanted_ = attackOn_ = faceRequest_ = false;
    attackError_ = 0;
    lootGuid_ = 0;
    lootWindow = LootWindow();
    lastLoot.clear();
    gossip = GossipMenu();
    corpse = CorpseInfo();
    resurrectOffer = ResurrectOffer();
    spiritHealerConfirm = 0;
    reclaimAt_ = corpseAskedMs_ = invScanMs_ = 0;
    lastLife_ = Life::Alive;
    lifeSeen_ = false;
    itemAsked_.clear();   // the item cache (items_) survives: static data
    itemWanted_.clear();
    spells.clear();
    actionBar = {};
    actionBarSeen = false;
    cast = CastState();
    cooldownEnd_.clear();
    unitCasts_.clear();
    auraTime_ = {};
    gcdEnd_ = 0;
    lastCastFail = 0xFFFFFFFFu;
    rep = ReputationState();   // the faction table (factions.txt) stays: static data
    vendor = VendorWindow();
    questDialog = QuestDialog();
    questStatus_.clear();
    statusAskedMs_.clear();
    questAsked_.clear();      // the quest cache (quests_) survives: static data
    questWanted_.clear();
    questLogIds_.clear();
    questLogSeen_ = false;
    questLogHash_ = questLevel_ = statusRefreshMs_ = 0;
    pendingItemLines_.clear();
    pendingSells_.clear();
    pendingRepair_ = PendingRepair();
    trainer = TrainerWindow();
    binderConfirm = 0;
    party = Party();
    groupInviteFrom.clear();
    bindPending_ = false;
    friends.clear();
    ignored.clear();
    friendsSeen = false;
    guild = GuildState();
    guildInviteFrom.clear();
    guildInviteName.clear();
    bank = BankWindow();
    taxi = TaxiWindow();
    mail = MailWindow();
    newMail = false;
    auction = AuctionWindow();
    trade = TradeState();
    mailTextAsked_.clear();
    for (MirrorTimer& m : mirror) m = MirrorTimer();
    envDamageTaken = lastEnvDamageMs = 0;
    fishBiteMs = 0;
    fishBiteGuid = 0;
    const float* p = wc.position();
    mover = Mover();   // a new session: default speeds, no water walking / walk mode from the last one
    mover.reset(p[0], p[1], p[2], wc.orientation(), nowMs);
    wc.onPacket = [this](const Packet& pk) { handle(pk.opcode, pk.data, now_); };
    sendActiveMover(nowMs);
}

const WorldObject* GameSession::selfObject() const {
    if (self_) return objects.find(self_);
    for (auto& [g, o] : objects.all())
        if (o.self) return &o;
    return nullptr;
}

void GameSession::detach() {
    if (wc_) wc_->onPacket = nullptr;
    wc_ = nullptr;
}

void GameSession::sendActiveMover(uint32_t nowMs) {
    Writer w;
    w.u64(self_);
    send(CMSG_SET_ACTIVE_MOVER, w.b, nowMs);
}

void GameSession::send(uint16_t op, const Bytes& data, uint32_t nowMs) {
    if (tap) tap(nowMs, 1, op, data);
    ++stats.packetsOut;
    if (wc_) wc_->send(op, data);
}

void GameSession::update(uint32_t nowMs) {
    now_ = nowMs;
    for (OutPacket& p : mover.queue()) send(p.opcode, p.data, nowMs);
    mover.queue().clear();
    sendQueries(nowMs);
    updateLife(nowMs);
    if (nowMs - invScanMs_ >= 1000 || !invScanMs_) {   // names for everything we carry (the bag page)
        invScanMs_ = nowMs ? nowMs : 1;
        for (const InvItem& it : inventory())
            if (it.entry) itemInfo(it.entry);
    }
    updateQuests(nowMs);
    updateVendor(nowMs);
    updateServices(nowMs);
    sendItemQueries(nowMs);
    // A cast whose SPELL_GO / failure never arrived must not block the bar for ever.
    if (cast.active && nowMs - cast.startMs > cast.durMs + 2000) cast = CastState();
}

bool GameSession::takeTransferPending(uint32_t* map) {
    if (!transferPending_) return false;
    transferPending_ = false;
    if (map) *map = transferMap_;
    return true;
}

bool GameSession::takeTeleport(float pos[3], float* orient) {
    if (!teleported_) return false;
    teleported_ = false;
    const MovementInfo& m = mover.info();
    pos[0] = m.x; pos[1] = m.y; pos[2] = m.z;
    if (orient) *orient = m.o;
    return true;
}

void GameSession::ackForced(uint16_t ackOp, uint64_t guid, uint32_t counter, uint32_t nowMs, const float* speed) {
    Writer w;
    w.u64(guid);
    w.u32(counter);
    writeMovementInfo(w, mover.snapshot(nowMs));
    if (speed) w.f32(*speed);
    send(ackOp, w.b, nowMs);
}

void GameSession::sendQueries(uint32_t nowMs) {
    for (uint64_t g : objects.takeCreated()) {
        const WorldObject* o = objects.find(g);
        if (!o || o->self) continue;
        Writer w;
        if (o->type == ObjType::Player) {
            if (objects.hasPlayerName(g) || !askedNames_.insert(g).second) continue;
            w.u64(g);
            send(ObjOp::CMSG_NAME_QUERY, w.b, nowMs);
        } else if (o->type == ObjType::Unit && o->entry()) {
            if (objects.hasCreatureInfo(o->entry()) || !askedCreatures_.insert(o->entry()).second) continue;
            w.u32(o->entry());
            w.u64(g);
            send(ObjOp::CMSG_CREATURE_QUERY, w.b, nowMs);
        } else if (o->type == ObjType::GameObject && o->entry()) {
            if (objects.hasGameObjectInfo(o->entry()) || !askedGos_.insert(o->entry()).second) continue;
            w.u32(o->entry());
            w.u64(g);
            send(ObjOp::CMSG_GAMEOBJECT_QUERY, w.b, nowMs);
        } else {
            continue;
        }
        ++stats.queries;
    }
}

bool GameSession::handle(uint16_t op, const Bytes& data, uint32_t nowMs) {
    if (tap) tap(nowMs, 0, op, data);
    if (objects.handle(op, data, nowMs)) {
        if (!self_ && !wc_)   // replays: take our GUID from the object the server flagged as SELF
            if (const WorldObject* me = selfObject()) { self_ = me->guid; objects.setSelf(self_); }
        return true;
    }
    Reader r(data);
    switch (op) {
        case MoveOp::TeleportAck: {   // server-initiated teleport within the map
            uint64_t g = r.packedGuid();
            uint32_t counter = r.u32();
            MovementInfo m;
            if (!readMovementInfo(r, m)) return true;
            if (g != self_) {
                if (WorldObject* o = objects.find(g)) { o->mv = m; o->posMs = nowMs; o->pathActive = false; }
                return true;
            }
            mover.reset(m.x, m.y, m.z, m.o, nowMs);
            Writer w;
            w.u64(self_);
            w.u32(counter);
            w.u32(nowMs);
            send(MoveOp::TeleportAck, w.b, nowMs);
            // The server set our position itself and ignores movement until this ACK;
            // anything still queued was measured from the old spot.
            mover.queue().clear();
            teleported_ = true;
            ++stats.teleports;
            return true;
        }
        case SMSG_NEW_WORLD: {
            map_ = r.u32();
            float x = r.f32(), y = r.f32(), z = r.f32(), o = r.f32();
            if (r.bad) return true;
            objects.clear();
            objects.setSelf(self_);
            mover.reset(x, y, z, o, nowMs);
            send(MoveOp::WorldportAck, {}, nowMs);
            sendActiveMover(nowMs);
            teleported_ = true;
            ++stats.newWorlds;
            return true;
        }
        case SMSG_TRANSFER_PENDING: {   // u32 map [, u32 transport entry, u32 transport map]: a new map is being loaded
            uint32_t m = r.u32();
            if (r.bad) return true;
            transferMap_ = m;
            transferPending_ = true;
            return true;
        }
        case SMSG_FORCE_RUN_SPEED_CHANGE:
        case SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
        case SMSG_FORCE_SWIM_SPEED_CHANGE:
        case SMSG_FORCE_WALK_SPEED_CHANGE:
        case SMSG_FORCE_SWIM_BACK_SPEED_CHANGE: {
            uint64_t g = readGuidBySize(r, 8);
            uint32_t counter = r.u32();
            float speed = r.f32();
            if (r.bad || !(speed > 0.1f && speed < 100.0f)) return true;
            if (g != self_) return true;
            MoveSpeeds& s = mover.speeds;
            uint16_t ack = 0;
            switch (op) {
                case SMSG_FORCE_RUN_SPEED_CHANGE: s.run = speed; ack = CMSG_FORCE_RUN_SPEED_CHANGE_ACK; break;
                case SMSG_FORCE_RUN_BACK_SPEED_CHANGE: s.runBack = speed; ack = CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK; break;
                case SMSG_FORCE_SWIM_SPEED_CHANGE: s.swim = speed; ack = CMSG_FORCE_SWIM_SPEED_CHANGE_ACK; break;
                case SMSG_FORCE_WALK_SPEED_CHANGE: s.walk = speed; ack = CMSG_FORCE_WALK_SPEED_CHANGE_ACK; break;
                default: s.swimBack = speed; ack = CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK; break;
            }
            ackForced(ack, self_, counter, nowMs, &speed);
            ++stats.speedChanges;
            return true;
        }
        case SMSG_FORCE_MOVE_ROOT:
        case SMSG_FORCE_MOVE_UNROOT: {
            uint64_t g = readGuidBySize(r, 4);
            uint32_t counter = r.u32();
            if (r.bad || g != self_) return true;
            bool root = op == SMSG_FORCE_MOVE_ROOT;
            mover.setRooted(root);
            ackForced(root ? CMSG_FORCE_MOVE_ROOT_ACK : CMSG_FORCE_MOVE_UNROOT_ACK, self_, counter, nowMs, nullptr);
            ++stats.roots;
            return true;
        }
        default:
            return handleSocial(op, data, nowMs) || handleSpells(op, data, nowMs) || handleItems(op, data, nowMs) ||
                   handleLife(op, data, nowMs) || handleFactions(op, data, nowMs) || handleVendor(op, data, nowMs) ||
                   handleQuests(op, data, nowMs) || handleGroup(op, data, nowMs) || handleFriends(op, data, nowMs) ||
                   handleServices(op, data, nowMs) || handleMail(op, data, nowMs) || handleAuction(op, data, nowMs) ||
                   handleTrade(op, data, nowMs) || handleWater(op, data, nowMs);
    }
}

// ------------------------------------------------------------------ packet log
bool PacketLog::open(const char* path) {
    close();
    f_ = fopen(path, "wb");
    if (!f_) return false;
    setvbuf(f_, nullptr, _IOFBF, 64 * 1024);   // SD card: write in big chunks
    fwrite("W3DL", 1, 4, f_);
    return true;
}

void PacketLog::write(uint32_t ms, int dir, uint16_t op, const Bytes& data) {
    if (!f_) return;
    uint8_t h[11];
    uint32_t len = (uint32_t)data.size();
    memcpy(h, &ms, 4);
    h[4] = (uint8_t)dir;
    memcpy(h + 5, &op, 2);
    memcpy(h + 7, &len, 4);
    fwrite(h, 1, sizeof h, f_);
    if (len) fwrite(data.data(), 1, len, f_);
}

void PacketLog::close() {
    if (f_) fclose(f_);
    f_ = nullptr;
}

bool readPacketLog(const char* path, std::vector<LoggedPacket>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "W3DL", 4) == 0;
    uint8_t h[11];
    while (ok && fread(h, 1, sizeof h, f) == sizeof h) {
        LoggedPacket p;
        uint32_t len;
        memcpy(&p.ms, h, 4);
        p.dir = h[4];
        memcpy(&p.opcode, h + 5, 2);
        memcpy(&len, h + 7, 4);
        if (len > (16u << 20)) { ok = false; break; }
        p.data.resize(len);
        if (len && fread(p.data.data(), 1, len, f) != len) { ok = false; break; }
        out.push_back(std::move(p));
    }
    fclose(f);
    return ok;
}

}  // namespace w3n
