// chat_test — stage 5.46: system messages, channels (slots, notices, list) and slash commands, with hand-built packets.
#include <cstdio>
#include <cstring>
#include <string>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<std::pair<uint16_t, Bytes>> sent;
static std::string last(GameSession& s) { return s.chat.empty() ? std::string("<none>") : s.lineText(s.chat.back()); }
static bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

static void notify(GameSession& s, uint8_t kind, const char* ch, std::function<void(Writer&)> extra, uint32_t ms = 100) {
    Writer w;
    w.u8(kind);
    w.cstr(ch);
    if (extra) extra(w);
    s.handle(0x099, w.b, ms);
}

int main() {
    GameSession s;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) sent.push_back({op, d}); };
    s.areaName = [](uint32_t id) { return id == 12 ? std::string("Elwynn Forest") : std::string(); };
    uint32_t ms = 100;

    // ---- channel slots
    notify(s, 0x02, "General - Elwynn Forest", [](Writer& w) { w.u32(0x18); w.u32(0); });
    CHECK(s.channelNumber("General - Elwynn Forest") == 1, "General takes slot 1");
    CHECK(last(s) == "Joined Channel: [1. General - Elwynn Forest]", "join text: %s", last(s).c_str());
    notify(s, 0x02, "Trade - Stormwind City", [](Writer& w) { w.u32(0x18); w.u32(0); });
    notify(s, 0x02, "Trade - Stormwind City", [](Writer& w) { w.u32(0x18); w.u32(0); });
    CHECK(s.channelNumber("Trade - Stormwind City") == 2 && s.channelSlots().size() == 2, "Trade is slot 2, a repeat does not duplicate");
    notify(s, 0x03, "General - Elwynn Forest", nullptr);
    CHECK(s.channelNumber("General - Elwynn Forest") == 0 && s.channelSlots().size() == 2 && s.channelNumber("Trade - Stormwind City") == 2,
          "leaving slot 1 keeps slot 2");
    CHECK(has(last(s), "Left Channel: [1. General"), "left text: %s", last(s).c_str());
    notify(s, 0x02, "Forest", [](Writer& w) { w.u32(0); w.u32(0); });
    CHECK(s.channelNumber("Forest") == 1, "the freed slot 1 is taken again");

    // ---- a channel message shows the number
    {
        Writer w;
        w.u8(Chat::Channel); w.u32(0); w.cstr("Trade - Stormwind City"); w.u32(0); w.u64(0x55); w.u32(6); w.raw("WTS ax", 6); w.u8(0);
        s.handle(0x096, w.b, ms);
        CHECK(has(last(s), "[2. Trade - Stormwind City] [") && has(last(s), "]: WTS ax"), "channel line: %s", last(s).c_str());
    }

    // ---- every notice of the table has a text
    for (int k = 0; k <= 0x1F; ++k) {
        std::string t = GameSession::channelNoticeText((uint8_t)k, "Chan", "Alice", "Bob");
        CHECK(!t.empty() && !has(t, "Channel notice 0x"), "notice %02X has text: %s", k, t.c_str());
    }
    CHECK(GameSession::channelNoticeText(0x12, "Chan", "Alice", "Bob") == "[Chan] Player Alice kicked by Bob.", "kick text");
    CHECK(GameSession::channelNoticeText(0x04, "Chan", "", "") == "Wrong password for Chan.", "password text");
    CHECK(has(GameSession::channelNoticeText(0x7F, "Chan", "", ""), "Channel notice 0x7F"), "unknown notice id");

    // notices with a payload
    notify(s, 0x12, "Trade - Stormwind City", [](Writer& w) { w.u64(0x71); w.u64(0x72); });
    CHECK(has(last(s), "kicked by") && s.chat.back().type == Chat::ChannelNotice, "kick notice: %s", last(s).c_str());
    notify(s, 0x09, "Forest", [](Writer& w) { w.cstr("Nobody"); });
    CHECK(last(s) == "[Forest] Player Nobody was not found.", "not found: %s", last(s).c_str());
    notify(s, 0x0C, "Forest", [](Writer& w) { w.u64(0x71); w.u8(0); w.u8(2); });
    CHECK(has(last(s), "is now a moderator"), "mode change: %s", last(s).c_str());
    notify(s, 0x11, "Forest", nullptr);
    CHECK(has(last(s), "permission to speak"), "muted: %s", last(s).c_str());
    size_t before = s.chat.size();
    { Writer w; w.u8(0x12); s.handle(0x099, w.b, ms); }   // truncated packet: no line, no crash
    CHECK(s.chat.size() == before, "a truncated notice adds nothing");

    // ---- the channel list
    {
        Writer w;
        w.cstr("Forest"); w.u8(1); w.u32(2); w.u64(0x71); w.u8(1); w.u64(0x72); w.u8(2);
        s.handle(0x09B, w.b, ms);
        CHECK(has(last(s), "Players in [1. Forest] (2): ") && has(last(s), "(owner)") && has(last(s), "(moderator)"), "list: %s", last(s).c_str());
    }

    // ---- system messages
    auto sys = [&](uint16_t op, std::function<void(Writer&)> f) { Writer w; f(w); s.handle(op, w.b, ms); };
    sys(0x291, [](Writer& w) { w.u32(2); w.cstr("5 minutes"); });
    CHECK(last(s) == "Server restart in 5 minutes." && s.chat.back().type == Chat::System, "server restart: %s", last(s).c_str());
    sys(0x291, [](Writer& w) { w.u32(1); w.cstr("30 seconds"); });
    CHECK(last(s) == "Server shutdown in 30 seconds.", "shutdown");
    sys(0x291, [](Writer& w) { w.u32(4); w.cstr(""); });
    CHECK(last(s) == "Server shutdown cancelled.", "cancel");
    sys(0x291, [](Writer& w) { w.u32(3); w.cstr("Hotfix applied"); });
    CHECK(last(s) == "[Server] Hotfix applied", "custom");
    sys(0x219, [](Writer&) {});
    CHECK(has(last(s), "opposing faction"), "wrong faction: %s", last(s).c_str());
    sys(0x2FD, [](Writer&) {});
    CHECK(has(last(s), "restricted"), "restricted: %s", last(s).c_str());
    sys(0x32D, [](Writer& w) { w.cstr("Bo"); });
    CHECK(last(s) == "Ambiguous player name 'Bo'.", "ambiguous: %s", last(s).c_str());
    sys(0x254, [](Writer& w) { w.u32(12); });
    CHECK(last(s) == "Elwynn Forest is under attack!", "under attack: %s", last(s).c_str());
    sys(0x2FA, [](Writer& w) { w.u32(2); w.u32(409); w.u32(30); });
    CHECK(has(last(s), "reset in 30 minute"), "raid instance: %s", last(s).c_str());
    sys(0x1CB, [](Writer& w) { w.cstr("Your bags are full"); });
    CHECK(last(s) == "Your bags are full", "notification");
    sys(0x2B8, [](Writer& w) { w.u32(10); w.cstr("You must be level 55 to enter."); });
    CHECK(last(s) == "You must be level 55 to enter.", "area trigger message");
    sys(0x096, [](Writer& w) { w.u8(Chat::System); w.u32(0); w.u64(0); w.u32(8); w.raw("Welcome!", 8); w.u8(0); });
    CHECK(last(s) == "Welcome!" && s.chat.back().type == Chat::System, "system chat line (MOTD): %s", last(s).c_str());
    // /who result and a roll
    sys(0x063, [](Writer& w) {
        w.u32(1); w.u32(5); w.cstr("Bob"); w.cstr("Knights"); w.u32(60); w.u32(1); w.u32(1); w.u32(12);
    });
    CHECK(has(last(s), "[Bob] <Knights> - Level 60 ") && has(last(s), "Elwynn Forest"), "who entry: %s", last(s).c_str());
    CHECK(s.chat.size() >= 2 && s.lineText(s.chat[s.chat.size() - 2]) == "5 players total, 1 shown.", "who header: %s", s.lineText(s.chat[s.chat.size() - 2]).c_str());
    sys(0x1FB, [](Writer& w) { w.u32(1); w.u32(100); w.u32(42); w.u64(0x71); });
    CHECK(has(last(s), "rolls 42 (1-100)"), "roll: %s", last(s).c_str());

    // ---- commands
    auto op = [&](const char* line) { sent.clear(); return s.sendTyped(line, ms); };
    auto cstrAt = [](const Bytes& b, size_t pos) { return std::string((const char*)b.data() + pos); };
    CHECK(op("/w Bob hello there") && sent.size() == 1 && sent[0].first == 0x095, "whisper sent");
    if (sent.size() == 1) {
        Reader r(sent[0].second);
        CHECK(r.u32() == Chat::Whisper, "type whisper");
        r.u32();
        CHECK(cstrAt(sent[0].second, 8) == "Bob" && cstrAt(sent[0].second, 12) == "hello there", "whisper payload");
    }
    CHECK(op("/whisper Ann hi") && sent.size() == 1, "/whisper long form");
    CHECK(op("/r back") && sent.size() == 1 && cstrAt(sent[0].second, 8) == "Ann", "/r answers the last target");
    CHECK(!op("/w Bob") && sent.empty() && has(last(s), "Usage"), "/w without text");
    CHECK(op("/raid go go") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::Raid, "/raid");
    CHECK(op("/rw pull") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::RaidWarning, "/rw");
    CHECK(op("/o hi") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::Officer, "/o");
    CHECK(op("/sh hey") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::Yell, "/sh = yell");
    CHECK(op("/afk gone fishing") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::Afk, "/afk");
    CHECK(op("just talking") && sent.size() == 1 && Reader(sent[0].second).u32() == Chat::Say, "plain text = say");
    CHECK(!op("/s") && sent.empty(), "/s needs text");

    CHECK(op("/join MyChan secret") && sent.size() == 1 && sent[0].first == 0x097 && cstrAt(sent[0].second, 0) == "MyChan" && cstrAt(sent[0].second, 7) == "secret", "/join with password");
    CHECK(op("/2 selling") && sent.size() == 1 && sent[0].first == 0x095, "/2 text goes to channel 2");
    if (sent.size() == 1) CHECK(cstrAt(sent[0].second, 8) == "Trade - Stormwind City" && cstrAt(sent[0].second, 8 + 23) == "selling", "channel payload");
    CHECK(!op("/5 hi") && has(last(s), "Not in channel 5"), "/5 without channel 5");
    CHECK(op("/leave trade") && sent.size() == 1 && sent[0].first == 0x098 && cstrAt(sent[0].second, 0) == "Trade - Stormwind City", "/leave by name prefix");
    CHECK(op("/chatlist 1") && sent.size() == 1 && sent[0].first == 0x09A && cstrAt(sent[0].second, 0) == "Forest", "/chatlist 1");
    CHECK(op("/ckick 2 Bob") && sent.size() == 1 && sent[0].first == 0x0A4, "/ckick");
    if (sent.size() == 1) CHECK(cstrAt(sent[0].second, 0) == "Trade - Stormwind City" && cstrAt(sent[0].second, 23) == "Bob", "ckick payload");
    CHECK(op("/cannounce 1") && sent.size() == 1 && sent[0].first == 0x0A7 && sent[0].second.size() == 7, "/cannounce");
    CHECK(!op("/ckick 9 Bob") && sent.empty() && has(last(s), "Not in channel 9"), "/ckick unknown channel");
    CHECK(op("/chat") && has(last(s), "Trade"), "/chat lists channels: %s", last(s).c_str());

    CHECK(op("/roll 50") && sent.size() == 1 && sent[0].first == 0x1FB && sent[0].second.size() == 8, "/roll 50");
    if (sent.size() == 1) { Reader r(sent[0].second); CHECK(r.u32() == 1 && r.u32() == 50, "roll 1..50"); }
    CHECK(op("/roll 5 10") && sent.size() == 1, "/roll 5 10");
    if (sent.size() == 1) { Reader r(sent[0].second); CHECK(r.u32() == 5 && r.u32() == 10, "roll 5..10"); }
    CHECK(op("/roll") && sent.size() == 1, "/roll default");
    CHECK(!op("/roll 10 5") && sent.empty(), "/roll min > max");
    CHECK(!op("/roll abc") && sent.empty(), "/roll abc");
    CHECK(op("/who elf") && sent.size() == 1 && sent[0].first == 0x062, "/who elf");
    if (sent.size() == 1) CHECK(has(std::string((const char*)sent[0].second.data(), sent[0].second.size()), "elf"), "who term in the packet");
    CHECK(op("/who") && sent.size() == 1, "/who");

    size_t n0 = s.chat.size();
    CHECK(op("/emotes") && sent.empty() && s.chat.size() > n0 && has(last(s), "/"), "/emotes lists");
    n0 = s.chat.size();
    CHECK(op("/help") && s.chat.size() == n0 + GameSession::commandHelp().size(), "/help");
    CHECK(op("/dance") && sent.size() == 1 && sent[0].first == 0x104, "/dance");
    CHECK(op("/ignore Zed") && sent.size() == 1, "/ignore sends");
    CHECK(op("/ginvite Zed") && sent.size() == 1, "/ginvite");
    CHECK(!op("/nonsense") && has(last(s), "Unknown command /nonsense"), "unknown command");
    CHECK(!op("/unfriend Zed") && has(last(s), "not on your friends"), "/unfriend unknown");
    CHECK(!op("/target Nobody") && has(last(s), "No unit named"), "/target unknown");
    CHECK(!op("   ") && sent.empty(), "blank line");

    // ids line up with the rest of the client
    CHECK(Chat::ChannelNotice == 0x12 && Chat::Afk == 0x14, "chat ids");

    std::printf("chat_test: %d checks, %d failed\n", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
