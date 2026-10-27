// Stage 5.6/5.7: mutated server packets (from a recorded fight/chat session) through the whole
// GameSession::handle (objects, chat, melee, xp, loot) under ASan/UBSan: no crash, bounded state.
//   social_fuzz <session.w3dl> [rounds] [--no-cycle] [--factions FILE]
// Stage 5.10 adds items, loot window, gossip, death/resurrection and the movement flag ACK handlers, and calls
// the UI-side queries (inventory, life, interact, item names) after every mutated session.
// Stages 5.12-5.14 add the faction, vendor and quest handlers and their queries (reactions, reputation list, quest
// log and progress, buyback), with the faction table of the test loaded when --factions is given.
#include "../game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using namespace w3n;

int main(int argc, char** argv) {
    std::vector<LoggedPacket> pk;
    if (argc < 2 || !readPacketLog(argv[1], pk)) { std::printf("cannot read log\n"); return 2; }
    int rounds = argc > 2 ? std::atoi(argv[2]) : 300;
    const bool cycle = !(argc > 3 && std::string(argv[3]) == "--no-cycle");
    std::string factionsFile;
    for (int i = 3; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--factions") factionsFile = argv[i + 1];
    if (cycle) {   // target cycling (L on the 3DS) over the recorded world: every unit in range once per cycle,
        // living creatures without NPC flags before NPCs, players and the dead
        GameSession s;
        uint32_t last = 0;
        for (const LoggedPacket& p : pk) if (p.dir == 0) { s.handle(p.opcode, p.data, p.ms); last = p.ms; }
        const float pos[3] = {-8949.95f, -132.49f, 83.53f};
        const float range = 30.0f;
        size_t inRange = 0;
        for (auto& [g, o] : s.objects.all()) {
            if (o.self || !o.hasPos || !o.isUnitLike()) continue;
            float q[3];
            o.positionAt(last, q);
            float d = std::sqrt((q[0] - pos[0]) * (q[0] - pos[0]) + (q[1] - pos[1]) * (q[1] - pos[1]) + (q[2] - pos[2]) * (q[2] - pos[2]));
            inRange += d <= range;
        }
        std::vector<uint64_t> seen;
        uint64_t cur = 0;
        int rank = 0, rankDrops = 0;
        for (size_t k = 0; k < inRange; ++k) {
            cur = s.pickTarget(pos, 0.35f, range, cur, last);
            if (!cur || std::find(seen.begin(), seen.end(), cur) != seen.end()) break;
            seen.push_back(cur);
            const WorldObject* o = s.objects.find(cur);
            int r = o->dead() ? 3 : o->type == ObjType::Player ? 2 : o->field(UF::UNIT_NPC_FLAGS) ? 1 : 0;
            if (r < rank) ++rankDrops;
            rank = r;
        }
        bool wraps = s.pickTarget(pos, 0.35f, range, cur, last) == seen.front();
        if (seen.size() != inRange || rankDrops || !wraps || inRange < 5) {
            std::printf("FAIL target cycle: %zu of %zu units, %d order breaks, wraps %d\n", seen.size(), inRange, rankDrops, wraps);
            return 1;
        }
        std::printf("target cycle: %zu units within %.0f yd, each once, creatures before NPCs/players/dead, wraps\n", inRange, range);
    }
    std::mt19937 rng(5875);
    size_t fed = 0, social = 0;
    const uint16_t socialOps[] = {0x096, 0x099, 0x143, 0x144, 0x145, 0x146, 0x14A, 0x14E, 0x160, 0x161, 0x162,
                                  0x165, 0x1CB, 0x1D0, 0x2A9,
                                  // stage 5.10
                                  0x058, 0x112, 0x163, 0x166, 0x15B, 0x17D, 0x17E, 0x180, 0x216, 0x222, 0x252, 0x269,
                                  0x2BD, 0x0DE, 0x0DF, 0x0F2, 0x0F3, 0x0F4, 0x0F5, 0x130, 0x12B,
                                  // stages 5.12-5.14
                                  0x122, 0x123, 0x124, 0x2A5, 0x313, 0x19F, 0x1A1, 0x1A4, 0x1A5, 0x05D, 0x183, 0x185,
                                  0x188, 0x18B, 0x18D, 0x18F, 0x191, 0x192, 0x195, 0x196, 0x197, 0x198, 0x199, 0x19A,
                                  0x1B1, 0x1B3, 0x1B4, 0x2EB, 0x1B6, 0x155, 0x137,
                                  // stage 5.17
                                  0x06F, 0x074, 0x077, 0x079, 0x07C, 0x07D, 0x07E, 0x07F, 0x2F2};
    FactionTable ft;
    if (!factionsFile.empty() && !ft.load(factionsFile.c_str())) { std::printf("cannot read %s\n", factionsFile.c_str()); return 2; }
    for (int round = 0; round < rounds; ++round) {
        GameSession s;
        s.factions = ft;
        for (const LoggedPacket& p : pk) {
            if (p.dir != 0) continue;
            Bytes d = p.data;
            uint16_t op = p.opcode;
            int kind = rng() % 6;
            if (kind == 0 && !d.empty()) d.resize(rng() % d.size());                         // truncate
            else if (kind == 1) for (int k = 0; k < 4 && !d.empty(); ++k) d[rng() % d.size()] ^= (uint8_t)(1u << (rng() % 8));
            else if (kind == 2) d.push_back((uint8_t)rng());                                 // trailing byte
            else if (kind == 3) op = socialOps[rng() % (sizeof socialOps / 2)];              // wrong handler
            else if (kind == 4 && d.size() > 4) for (size_t k = 1; k < 5; ++k) d[k] = 0xFF;  // huge lengths/counts
            s.handle(op, d, p.ms);
            s.objects.takeCreated();
            ++fed;
            for (uint16_t so : socialOps) social += so == op;
        }
        if (s.chat.size() > GameSession::kChatKeep || s.hits.size() > 32) { std::printf("FAIL: unbounded state\n"); return 1; }
        for (const ChatLine& l : s.chat) (void)s.lineText(l);
        s.update(1000000);
        const float pos[3] = {-8949.95f, -132.49f, 83.53f};
        for (const InvItem& it : s.inventory()) (void)s.itemName(it.entry);
        (void)s.life();
        (void)s.interact(s.target(), pos, 1000000);
        (void)s.corpseDistance(pos);
        (void)s.freeBagSlots();
        if (s.gossip.options.size() > 32 || s.gossip.quests.size() > 32) { std::printf("FAIL: gossip unbounded\n"); return 1; }
        for (auto& [g, o] : s.objects.all())
            if (o.isUnitLike()) {
                (void)s.shownReaction(o); (void)s.canAttack(o); (void)s.enemyLike(o); (void)s.questGiverStatus(g);
                if (s.auras(o, 1000000).size() > 48) { std::printf("FAIL: auras unbounded\n"); return 1; }
            }
        (void)s.reputations();
        if (s.party.members.size() > 40) { std::printf("FAIL: party unbounded\n"); return 1; }
        for (const PartyMember& m : s.party.members) (void)s.memberView(m);
        for (const QuestLogEntry& e : s.questLog()) { (void)s.questTitle(e.id); (void)s.questProgress(e); }
        (void)s.buybackItems();
        for (size_t i = 0; i < s.trainer.spells.size() && i < 4; ++i) (void)s.trainSpell(i, 1000500);
        if (s.trainer.spells.size() > 512) { std::printf("FAIL: trainer unbounded\n"); return 1; }
        if (s.vendor.items.size() > 255 || s.questDialog.list.size() > 32 || s.questDialog.choices.size() > 6 ||
            s.questDialog.rewards.size() > 4 || s.questDialog.required.size() > 6 || s.questLog().size() > 20) {
            std::printf("FAIL: vendor / quest window unbounded\n");
            return 1;
        }
        s.update(1001000);
    }
    std::printf("social fuzz: %zu mutated packets (%zu to chat/melee/loot handlers) without crashing\n", fed, social);
    return 0;
}
