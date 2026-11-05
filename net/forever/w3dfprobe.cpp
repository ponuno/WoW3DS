// w3dfprobe — WoW Forever login probe on the PC (step 1 of the Forever port, see docs/FOREVER_RU.md): logs into a private
// Forever server (TrinityCore Forever fork) the way the 1.60.1 client does - REST login, Battle.net RPC, realm list, realm join,
// world connection with AES-GCM - and asks for the character list. Writes a readable report and a packet log for the next steps.
//   w3dfprobe <server> <e-mail> [--realm N] [--build 70658] [--rest-port 8081] [--bnet-port 1119] [--rest-http] [--ca file.pem]
//             [--create NAME [--race 1] [--class 1] [--sex 0]] [--enter] [--stay 20] [--out report.txt] [--log packets.fwl]          (the password is read from the console)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fbnet.hpp"
#include "fworld.hpp"

#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace wf;

static FILE* g_out = nullptr;
static void line(const std::string& s) {
    printf("%s\n", s.c_str());
    fflush(stdout);
    if (g_out) { fprintf(g_out, "%s\n", s.c_str()); fflush(g_out); }
}

static std::string readPassword() {
    std::string p;
    fprintf(stderr, "Password: ");
    fflush(stderr);
#ifdef _WIN32
    for (;;) {
        int c = _getch();
        if (c == '\r' || c == '\n' || c == EOF) break;
        if (c == 8) { if (!p.empty()) p.pop_back(); continue; }
        p += (char)c;
    }
#else
    termios old{}, now{};
    const bool tty = isatty(0) && tcgetattr(0, &old) == 0;
    if (tty) { now = old; now.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSANOW, &now); }
    char buf[512];
    if (fgets(buf, sizeof buf, stdin)) p = buf;
    if (tty) tcsetattr(0, TCSANOW, &old);
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
#endif
    fprintf(stderr, "\n");
    return p;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: w3dfprobe <server> <e-mail> [--realm N] [--build 70658] [--rest-port 8081] [--bnet-port 1119]\n"
                        "                 [--create NAME [--race 1] [--class 1] [--sex 0]] [--enter]\n"
                        "                 [--rest-http] [--ca ca.pem] [--stay 20] [--out report.txt] [--log packets.fwl]\n"
                        "The password is asked for on the console (or read from stdin).\n");
        return 2;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    BnetConfig cfg;
    cfg.host = argv[1];
    cfg.email = argv[2];
    int realmIdx = -1, stay = 20;
    std::string createName;
    int createRace = 1, createClass = 1, createSex = 0;
    bool enterWorld = false;
    bool restGiven = false;
    std::string authKeyHex;
    int tries = 2;
    std::string outPath = "forever_probe.txt", logPath = "forever_packets.fwl";
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--realm") realmIdx = atoi(next());
        else if (a == "--build") cfg.build = (uint32_t)strtoul(next(), nullptr, 10);
        else if (a == "--rest-port") { cfg.restPort = atoi(next()); restGiven = true; }
        else if (a == "--bnet-port") cfg.bnetPort = atoi(next());
        else if (a == "--rest-http") cfg.restTls = false;
        else if (a == "--ca") cfg.caFile = next();
        else if (a == "--stay") stay = atoi(next());
        else if (a == "--create") createName = next();
        else if (a == "--race") createRace = atoi(next());
        else if (a == "--class") createClass = atoi(next());
        else if (a == "--sex") createSex = atoi(next());
        else if (a == "--enter") enterWorld = true;
        else if (a == "--out") outPath = next();
        else if (a == "--log") logPath = next();
        else if (a == "--platform") cfg.platform = next();
        else if (a == "--auth-key") authKeyHex = next();
        else if (a == "--tries") tries = atoi(next());
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    cfg.password = readPassword();
    g_out = fopen(outPath.c_str(), "w");
    line("w3dfprobe: WoW Forever login probe, client build 1.60.1." + std::to_string(cfg.build) + ", server " + cfg.host);
    BnetLogin bnet(cfg);
    bnet.log = [](const std::string& s) { line("  " + s); };
    std::string err, ticket;
    auto fail = [&](const char* step) {
        line(std::string("FAILED at ") + step + ": " + err);
        if (g_out) fclose(g_out);
        return 1;
    };
    line("[1/5] REST login");
    if (!restGiven) {   // the server names its login page itself (as for the real client)
        std::string url;
        if (!bnet.discoverRestUrl(url, err)) line("  could not ask the server for its login page (" + err + "), trying port " + std::to_string(cfg.restPort));
    }
    if (!bnet.restLogin(ticket, err)) return fail("REST login");
    line("  ticket " + ticket.substr(0, 6) + "... (" + std::to_string(ticket.size()) + " chars)");
    line("[2/5] Battle.net logon");
    if (!bnet.logon(ticket, err)) return fail("Battle.net logon");
    line("[3/5] realm list");
    std::vector<RealmInfo> realms;
    if (!bnet.realmList(realms, err)) return fail("realm list");
    for (size_t i = 0; i < realms.size(); ++i) {
        const RealmInfo& r = realms[i];
        char b[256];
        snprintf(b, sizeof b, "  %zu: %s  [%u-%u-%u] build %s, population %u, category %u, flags 0x%x, %d character(s)", i, r.name.c_str(),
                 r.address >> 24, (r.address >> 16) & 0xFF, r.address & 0xFFFF, r.versionText.c_str(), r.population, r.category, r.flags, r.characters);
        line(b);
    }
    if (realms.empty()) { err = "the list is empty"; return fail("realm list"); }
    if (realmIdx < 0) {   // the first realm of our build, else the first
        realmIdx = 0;
        for (size_t i = 0; i < realms.size(); ++i) if (realms[i].build == cfg.build) { realmIdx = (int)i; break; }
    }
    if (realmIdx >= (int)realms.size()) { err = "no realm " + std::to_string(realmIdx); return fail("realm list"); }
    Bytes authKey;
    for (size_t i = 0; i + 1 < authKeyHex.size(); i += 2) authKey.push_back((uint8_t)strtoul(authKeyHex.substr(i, 2).c_str(), nullptr, 16));
    if (!authKeyHex.empty() && authKey.size() != 16) { err = "--auth-key needs 32 hex digits"; return fail("options"); }
    PacketLogFile plog;
    plog.open(logPath);
    bool ok = false;
    for (int attempt = 1; attempt <= tries && !ok; ++attempt) {
    line("[4/5] joining realm " + std::to_string(realmIdx) + " (" + realms[(size_t)realmIdx].name + ")" + (attempt > 1 ? ", attempt " + std::to_string(attempt) : ""));
    JoinInfo join;
    if (!bnet.joinRealm(realms[(size_t)realmIdx], join, err)) return fail("realm join");
    line("[5/5] world server" + std::string(authKey.empty() ? " (digest with an all-zero build key)" : " (digest with the given build key)"));
    WorldProbe world;
    world.buildKey = authKey;
    world.createName = createName;
    world.createRace = (uint8_t)createRace;
    world.createClass = (uint8_t)createClass;
    world.createSex = (uint8_t)createSex;
    world.enter = enterWorld;
    world.log = [](const std::string& s) { line("  " + s); };
    world.onPacket = [&](const WorldPacket& p) {
        plog.write(p);
        const char* n = p.fromServer ? serverOpcodeName(p.opcode) : clientOpcodeName(p.opcode);
        char b[200];
        snprintf(b, sizeof b, "    %6u ms %s 0x%06X %-40s %6zu bytes%s  %s", p.ms, p.fromServer ? "S>" : "C>", p.opcode, n ? n : "?", p.data.size(),
                 p.compressed ? " (zlib)" : "", hex(p.data, 24).c_str());
        if (g_out) { fprintf(g_out, "%s\n", b); fflush(g_out); }
    };
    ok = world.run(join, stay, err);
    if (!ok) { line("  world: " + err); if (!world.closedAfterMs) { bnet.close(); return fail("world server"); } continue; }
    line("characters (names guessed from the raw bytes until the Classic layout is read):");
    for (const EnumCharacter& c : world.characters()) line("  " + c.name + "  (" + c.note + ")");
    if (world.characters().empty()) line("  none found");
    if (!createName.empty() && world.createResult() != 0xFFFFFFFF)
        line(std::string("character creation: SMSG_CREATE_CHAR code ") + std::to_string(world.createResult()) + (world.createResult() == 24 ? " (success)" : " (refused)"));
    if (enterWorld) line(world.enteredWorld() ? "world entry: SMSG_LOGIN_VERIFY_WORLD received - the character is in the world"
                                              : "world entry: no SMSG_LOGIN_VERIFY_WORLD (see the packet list above / packets.fwl)");
    line("DONE: login, realm list, realm join, encrypted world session and the character list work. " + std::to_string(world.packetsIn()) +
         " packets from the server; packet log " + logPath + ", this report " + outPath + ".");
    }
    bnet.close();
    if (!ok) return fail("world server (AUTH_SESSION refused every time)");
    if (g_out) fclose(g_out);
    return 0;
}
