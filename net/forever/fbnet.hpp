// fbnet.hpp — the Battle.net side of a WoW Forever login as the TrinityCore Forever fork serves it:
//   1. REST (https://host:8081/bnetserver/login/): the account's e-mail and password as a JSON form -> a login ticket "TC-...";
//   2. RPC over TLS (host:1119): each message = uint16 big-endian header size, bgs.protocol.Header (protobuf), the message;
//      ConnectionService.Connect, AuthenticationService.Logon (v1) with the ticket -> AuthenticationListener.OnLogonComplete
//      (the game accounts); GameUtilitiesService: RealmListTicketRequest (our 32-byte client secret), the sub-regions,
//      RealmListRequest (zlib JSON), RealmJoinRequest -> the join ticket, the world server's address and the server's secret.
// The world session key is derived from client secret + join secret (fworld).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "fbase.hpp"

namespace wf {

struct BnetConfig {
    std::string host;
    int restPort = 8081, bnetPort = 1119;
    bool restTls = true;
    std::string restHost, restPath = "/bnetserver/login/";   // (restHost empty: the server's own host)
    std::string caFile;            // check the servers' certificates against this CA (empty: not checked)
    std::string email, password;
    uint32_t build = 70658;        // 1.60.1.<build>
    std::string locale = "enUS";
    std::string platform = "Wn64";   // ClientBuild::Platform
    std::string arch = "x64", type = "WoW", platformType = "Win";
};

struct RealmInfo {
    uint32_t address = 0;          // region << 24 | battlegroup << 16 | index
    std::string name, subRegion;
    uint32_t flags = 0, population = 0, category = 0, build = 0;
    int characters = 0;
    std::string versionText;       // "1.60.1.70658"
};

struct JoinInfo {
    Bytes joinTicket;              // JSON RealmJoinTicket (the game account's name and the client variant)
    Bytes joinSecret;              // 32 bytes from the server
    Bytes clientSecret;            // 32 bytes we made
    std::vector<std::pair<std::string, int>> addresses;   // world server(s)
    uint32_t realmAddress = 0;
    std::string gameAccount;
};

class BnetLogin {
public:
    explicit BnetLogin(const BnetConfig& c);
    std::function<void(const std::string&)> log;   // progress lines
    // As the client does: Logon over RPC without credentials -> the server names its web login page (OnExternalChallenge,
    // "web_auth_url"); the REST login goes there. Sets restHost / restPort / restTls / restPath.
    bool discoverRestUrl(std::string& url, std::string& err);
    bool restLogin(std::string& ticket, std::string& err);
    // RPC: logon with the ticket and fetch the realm list
    bool logon(const std::string& ticket, std::string& err);
    bool realmList(std::vector<RealmInfo>& out, std::string& err);
    bool joinRealm(const RealmInfo& r, JoinInfo& out, std::string& err);
    void close();
    ~BnetLogin();
    uint64_t gameAccountId() const { return gameAccount_; }
    std::string rawRealmJson() const { return realmJson_; }
private:
    struct Rpc;
    BnetConfig cfg_;
    std::unique_ptr<Rpc> rpc_;
    uint64_t gameAccount_ = 0;
    Bytes clientSecret_, realmListTicket_;
    std::vector<std::string> subRegions_;
    std::string realmJson_;
    void say(const std::string& s) { if (log) log(s); }
};

}  // namespace wf
