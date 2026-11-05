// fbnet.cpp — see fbnet.hpp. Service hashes / method ids / field numbers are those of the bgs.protocol definitions the
// TrinityCore Forever fork compiles (src/server/proto); tools/forever/README.md tells how they were read.
#include "fbnet.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "fnet.hpp"

namespace wf {

namespace {
constexpr uint32_t kConnectionService = 0x65446991, kAuthenticationService = 0x0DECFC01, kGameUtilities = 0x3FC1274D;
constexpr uint32_t kAuthenticationListener = 0x71240E35, kChallengeListener = 0xBBDA171F;
constexpr uint32_t kProgramWoW = fourcc("WoW");

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// bgs.protocol.Variant
enum class VT { None, Bool, Int, Float, Str, Blob, Uint };
struct Variant {
    VT t = VT::None;
    bool b = false;
    int64_t i = 0;
    uint64_t u = 0;
    double f = 0;
    std::string s;   // string or blob
};
pb::W variantMsg(const Variant& v) {
    pb::W w;
    switch (v.t) {
        case VT::Bool: w.boolean(2, v.b); break;
        case VT::Int: w.u(3, (uint64_t)v.i); break;
        case VT::Str: w.str(5, v.s); break;
        case VT::Blob: w.bytes(6, v.s.data(), v.s.size()); break;
        case VT::Uint: w.u(9, v.u); break;
        default: break;
    }
    return w;
}
Variant parseVariant(const pb::Field& f) {
    Variant v;
    pb::each(f.data, f.len, [&](const pb::Field& x) {
        switch (x.num) {
            case 2: v.t = VT::Bool; v.b = x.v != 0; break;
            case 3: v.t = VT::Int; v.i = (int64_t)x.v; break;
            case 4: { v.t = VT::Float; double d; memcpy(&d, &x.v, 8); v.f = d; break; }
            case 5: v.t = VT::Str; v.s = x.str(); break;
            case 6: v.t = VT::Blob; v.s = x.str(); break;
            case 9: v.t = VT::Uint; v.u = x.v; break;
            default: break;
        }
    });
    return v;
}
using Attrs = std::vector<std::pair<std::string, Variant>>;
pb::W clientRequest(const Attrs& attrs) {
    pb::W w;
    for (auto& [name, v] : attrs) {
        pb::W a;
        a.str(1, name);
        a.msg(2, variantMsg(v));
        w.msg(1, a);
    }
    w.f32(5, kProgramWoW);
    return w;
}
Attrs parseAttrs(const Bytes& msg) {
    Attrs out;
    pb::each(msg, [&](const pb::Field& f) {
        if (f.num != 1 || f.wire != 2) return;
        std::string name;
        Variant v;
        pb::each(f.data, f.len, [&](const pb::Field& x) {
            if (x.num == 1) name = x.str();
            else if (x.num == 2) v = parseVariant(x);
        });
        out.emplace_back(name, v);
    });
    return out;
}
const Variant* findAttr(const Attrs& a, const std::string& name) {
    for (auto& kv : a) if (kv.first == name) return &kv.second;
    return nullptr;
}
Variant str(const std::string& s) { Variant v; v.t = VT::Str; v.s = s; return v; }
Variant blob(const std::string& s) { Variant v; v.t = VT::Blob; v.s = s; return v; }
Variant blob(const Bytes& b) { Variant v; v.t = VT::Blob; v.s.assign((const char*)b.data(), b.size()); return v; }
Variant uintv(uint64_t u) { Variant v; v.t = VT::Uint; v.u = u; return v; }

std::string attrsText(const Attrs& a) {
    std::string s;
    for (auto& [n, v] : a) {
        if (!s.empty()) s += ", ";
        s += n + "=";
        switch (v.t) {
            case VT::Str: s += "\"" + v.s + "\""; break;
            case VT::Blob: s += "blob[" + std::to_string(v.s.size()) + "]"; break;
            case VT::Uint: s += std::to_string(v.u); break;
            case VT::Int: s += std::to_string(v.i); break;
            case VT::Bool: s += v.b ? "true" : "false"; break;
            default: s += "?"; break;
        }
    }
    return s;
}
}  // namespace

// ---------------------------------------------------------------- RPC
struct BnetLogin::Rpc {
    Conn c;
    uint32_t nextToken = 1;
    struct Msg {
        bool response = false;
        uint32_t serviceId = 0, serviceHash = 0, method = 0, token = 0, status = 0;
        Bytes payload;
    };
    std::vector<Msg> incoming;   // requests of the server to us (listeners), in order

    bool send(uint32_t hash, uint32_t method, const pb::W& m, uint32_t& token, std::string& err) {
        token = nextToken++;
        pb::W h;
        h.u(1, 0);          // service_id: 0 = a request (0xFE = a response)
        h.u(2, method);
        h.u(3, token);
        h.u(5, m.b.size());
        h.f32(11, hash);
        Bytes frame;
        frame.push_back((uint8_t)(h.b.size() >> 8));
        frame.push_back((uint8_t)h.b.size());
        frame.insert(frame.end(), h.b.begin(), h.b.end());
        frame.insert(frame.end(), m.b.begin(), m.b.end());
        return c.writeAll(frame.data(), frame.size(), err);
    }
    bool recv(Msg& out, int timeoutMs, std::string& err) {
        uint8_t len[2];
        if (!c.readExact(len, 2, timeoutMs, err)) return false;
        Bytes hdr((size_t)(len[0] << 8 | len[1]));
        if (!hdr.empty() && !c.readExact(hdr.data(), hdr.size(), timeoutMs, err)) return false;
        uint32_t size = 0;
        out = Msg();
        if (!pb::each(hdr, [&](const pb::Field& f) {
                switch (f.num) {
                    case 1: out.serviceId = (uint32_t)f.v; break;
                    case 2: out.method = (uint32_t)f.v; break;
                    case 3: out.token = (uint32_t)f.v; break;
                    case 5: size = (uint32_t)f.v; break;
                    case 6: out.status = (uint32_t)f.v; break;
                    case 11: out.serviceHash = (uint32_t)f.v; break;
                    default: break;
                }
            })) { err = "bad RPC header " + hex(hdr); return false; }
        if (size > (4u << 20)) { err = "RPC message too big"; return false; }
        out.payload.resize(size);
        if (size && !c.readExact(out.payload.data(), size, timeoutMs, err)) return false;
        out.response = out.serviceId == 0xFE;
        return true;
    }
    // A call and its answer; the server's own requests that come in meanwhile are kept in 'incoming'.
    bool call(uint32_t hash, uint32_t method, const pb::W& m, Msg& answer, std::string& err, int timeoutMs = 20000) {
        uint32_t token;
        if (!send(hash, method, m, token, err)) return false;
        for (;;) {
            Msg msg;
            if (!recv(msg, timeoutMs, err)) { err = "waiting for the answer to " + std::to_string(method) + ": " + err; return false; }
            if (msg.response && msg.token == token) { answer = std::move(msg); return true; }
            if (!msg.response) incoming.push_back(std::move(msg));
        }
    }
    // The server's request to a listener (service hash + method), from 'incoming' or the connection.
    bool waitRequest(uint32_t hash, uint32_t method, Msg& out, std::string& err, int timeoutMs = 20000,
                     const std::function<bool(const Msg&)>& other = nullptr) {
        for (;;) {
            for (size_t i = 0; i < incoming.size(); ++i) {
                if (incoming[i].serviceHash == hash && incoming[i].method == method) {
                    out = std::move(incoming[i]);
                    incoming.erase(incoming.begin() + (long)i);
                    return true;
                }
                if (other && other(incoming[i])) { incoming.erase(incoming.begin() + (long)i); return false; }
            }
            Msg msg;
            if (!recv(msg, timeoutMs, err)) return false;
            if (!msg.response) incoming.push_back(std::move(msg));
        }
    }
};

BnetLogin::BnetLogin(const BnetConfig& c) : cfg_(c) {}
BnetLogin::~BnetLogin() { close(); }
void BnetLogin::close() { if (rpc_) rpc_->c.close(); rpc_.reset(); }

bool BnetLogin::discoverRestUrl(std::string& url, std::string& err) {
    Rpc rpc;
    say("Battle.net RPC: TLS to " + cfg_.host + ":" + std::to_string(cfg_.bnetPort) + " to ask for the login page");
    if (!rpc.c.open(cfg_.host, cfg_.bnetPort, true, err, cfg_.caFile)) return false;
    say("Battle.net RPC: " + rpc.c.tlsInfo());
    Rpc::Msg ans;
    pb::W con;
    con.boolean(3, true);
    if (!rpc.call(kConnectionService, 1, con, ans, err)) return false;
    pb::W req;
    req.str(1, "WoW");
    req.str(2, cfg_.platform);
    req.str(3, cfg_.locale);
    req.str(5, "1.60.1." + std::to_string(cfg_.build));
    req.u(6, cfg_.build);
    req.boolean(7, false);
    req.boolean(10, true);
    req.str(15, "{\"UTCO\":0}");
    if (!rpc.call(kAuthenticationService, 1, req, ans, err)) return false;
    if (ans.status) { err = "Logon refused, status " + std::to_string(ans.status); return false; }
    Rpc::Msg ch;
    if (!rpc.waitRequest(kChallengeListener, 3, ch, err, 15000)) { err = "no login page from the server: " + err; return false; }
    std::string type;
    pb::each(ch.payload, [&](const pb::Field& f) {
        if (f.num == 2) type = f.str();
        if (f.num == 3) url = f.str();
    });
    rpc.c.close();
    say("Battle.net RPC: login page (" + type + "): " + url);
    // https://host:port/path
    const size_t sch = url.find("://");
    if (sch == std::string::npos) { err = "unreadable login page address " + url; return false; }
    cfg_.restTls = url.compare(0, 5, "https") == 0;
    std::string rest = url.substr(sch + 3);
    const size_t slash = rest.find('/');
    cfg_.restPath = slash == std::string::npos ? "/" : rest.substr(slash);
    std::string hp = rest.substr(0, slash);
    const size_t colon = hp.rfind(':');
    cfg_.restHost = colon == std::string::npos ? hp : hp.substr(0, colon);
    cfg_.restPort = colon == std::string::npos ? (cfg_.restTls ? 443 : 80) : atoi(hp.c_str() + colon + 1);
    if (cfg_.restPath.empty() || cfg_.restPath.back() != '/') cfg_.restPath += "/";
    return true;
}

bool BnetLogin::restLogin(std::string& ticket, std::string& err) {
    const std::string body = "{\"platform_id\":\"" + jsonEsc(cfg_.platform) + "\",\"program_id\":\"WoW\",\"version\":\"1.60.1." +
                              std::to_string(cfg_.build) + "\",\"inputs\":[{\"input_id\":\"account_name\",\"value\":\"" + jsonEsc(cfg_.email) +
                              "\"},{\"input_id\":\"password\",\"value\":\"" + jsonEsc(cfg_.password) + "\"}]}";
    int status = 0;
    std::string resp;
    const std::string rh = cfg_.restHost.empty() ? cfg_.host : cfg_.restHost;
    say("REST login: POST " + std::string(cfg_.restTls ? "https" : "http") + "://" + rh + ":" + std::to_string(cfg_.restPort) + cfg_.restPath);
    bool posted = httpPost(rh, cfg_.restPort, cfg_.restTls, cfg_.restPath, body, status, resp, err, cfg_.caFile);
    if (!posted && rh != cfg_.host) {   // the page's host may be a name only the server's own network knows: the address we were given
        say("REST login: " + err + "; trying " + cfg_.host + " on the same port");
        posted = httpPost(cfg_.host, cfg_.restPort, cfg_.restTls, cfg_.restPath, body, status, resp, err, cfg_.caFile);
    }
    if (!posted) {
        // a server with the development certificate serves the REST login over plain HTTP: try that once
        std::string err2;
        if (!cfg_.restTls || err.find("TLS") == std::string::npos ||
            !httpPost(rh, cfg_.restPort, false, cfg_.restPath, body, status, resp, err2)) {
            err = "REST login: " + err;
            return false;
        }
        say("REST login: HTTPS failed (" + err + "), plain HTTP worked");
    }
    say("REST login: HTTP " + std::to_string(status) + " " + resp.substr(0, 300));
    Json j;
    std::string jerr;
    if (!Json::parse(resp, j, &jerr)) { err = "REST login: the answer is not JSON (" + jerr + ")"; return false; }
    ticket = j.strOr("login_ticket", "");
    if (ticket.empty()) {
        const std::string state = j.strOr("authentication_state", "?");
        err = "REST login refused (state " + state + (j.get("error_code") ? ", " + j.strOr("error_code", "") : std::string()) +
              (state == "AUTHENTICATOR" ? ": the account has an authenticator - not supported by the probe" :
               state == "DONE" ? ": wrong e-mail or password" : "") + ")";
        return false;
    }
    return true;
}

bool BnetLogin::logon(const std::string& ticket, std::string& err) {
    rpc_.reset(new Rpc);
    say("Battle.net RPC: TLS to " + cfg_.host + ":" + std::to_string(cfg_.bnetPort));
    if (!rpc_->c.open(cfg_.host, cfg_.bnetPort, true, err, cfg_.caFile)) return false;
    say("Battle.net RPC: " + rpc_->c.tlsInfo());
    Rpc::Msg ans;
    {   // ConnectionService.Connect (bindless RPC: the services are addressed by their hashes)
        pb::W req;
        req.boolean(3, true);
        if (!rpc_->call(kConnectionService, 1, req, ans, err)) return false;
        if (ans.status) { err = "Connect refused, status " + std::to_string(ans.status); return false; }
        say("Battle.net RPC: connected");
    }
    {   // AuthenticationService.Logon (v1) with the REST ticket as cached web credentials
        pb::W req;
        req.str(1, "WoW");
        req.str(2, cfg_.platform);
        req.str(3, cfg_.locale);
        req.str(5, "1.60.1." + std::to_string(cfg_.build));
        req.u(6, cfg_.build);
        req.boolean(7, false);
        req.boolean(10, true);
        req.bytes(12, ticket.data(), ticket.size());
        req.str(15, "{\"UTCO\":0}");
        if (!rpc_->call(kAuthenticationService, 1, req, ans, err)) return false;
        if (ans.status) { err = "Logon refused, status " + std::to_string(ans.status) + " (BattlenetRpcErrorCodes)"; return false; }
    }
    Rpc::Msg done;
    bool challenge = false;
    if (!rpc_->waitRequest(kAuthenticationListener, 5, done, err, 20000, [&](const Rpc::Msg& m) {
            if (m.serviceHash == kChallengeListener) { challenge = true; return true; }
            return false;
        })) {
        if (challenge) err = "the server asks for a web login again (the ticket was not accepted)";
        else err = "no OnLogonComplete: " + err;
        return false;
    }
    uint32_t code = 0;
    std::vector<uint64_t> gameAccounts;
    pb::each(done.payload, [&](const pb::Field& f) {
        if (f.num == 1) code = (uint32_t)f.v;
        if (f.num == 3 && f.wire == 2) {
            uint64_t low = 0;
            pb::each(f.data, f.len, [&](const pb::Field& x) { if (x.num == 2) low = x.v; });
            gameAccounts.push_back(low);
        }
    });
    if (code) { err = "OnLogonComplete error " + std::to_string(code); return false; }
    if (gameAccounts.empty()) { err = "the account has no game account (WoW license) on this server"; return false; }
    gameAccount_ = gameAccounts[0];
    say("Battle.net RPC: logged on, " + std::to_string(gameAccounts.size()) + " game account(s), using " + std::to_string(gameAccount_));
    return true;
}

bool BnetLogin::realmList(std::vector<RealmInfo>& out, std::string& err) {
    if (!rpc_) { err = "not logged on"; return false; }
    Rpc::Msg ans;
    {   // RealmListTicketRequest: who we are and our client (with the 32-byte secret the world key starts with)
        clientSecret_.resize(32);
        crypto::random(clientSecret_.data(), 32);
        std::string secret;
        for (size_t i = 0; i < 32; ++i) secret += (i ? "," : "") + std::to_string(clientSecret_[i]);
        const std::string identity = "JSONRealmListTicketIdentity:{\"gameAccountID\":" + std::to_string((uint32_t)gameAccount_) +
                                     ",\"gameAccountRegion\":2}";
        const std::string info = "JSONRealmListTicketClientInformation:{\"info\":{\"platform\":" + std::to_string(fourcc(cfg_.platform.c_str())) +
            ",\"buildVariant\":\"win-x64-wow\",\"type\":" + std::to_string(fourcc(cfg_.type.c_str())) + ",\"timeZone\":\"Etc/UTC\",\"currentTime\":" +
            std::to_string((uint32_t)time(nullptr)) + ",\"textLocale\":" + std::to_string(fourcc(cfg_.locale.c_str())) + ",\"audioLocale\":" +
            std::to_string(fourcc(cfg_.locale.c_str())) + ",\"versionDataBuild\":" + std::to_string(cfg_.build) +
            ",\"version\":{\"versionMajor\":1,\"versionMinor\":60,\"versionRevision\":1,\"versionBuild\":" + std::to_string(cfg_.build) +
            "},\"secret\":[" + secret + "],\"clientArch\":" + std::to_string(fourcc(cfg_.arch.c_str())) +
            ",\"systemVersion\":\"Windows 10\",\"platformType\":" + std::to_string(fourcc(cfg_.platformType.c_str())) + ",\"systemArch\":" +
            std::to_string(fourcc(cfg_.arch.c_str())) + "}}";
        Attrs a = {{"Command_RealmListTicketRequest_v1_b9", str("")}, {"Param_Identity", blob(identity)}, {"Param_ClientInfo", blob(info)}};
        if (!rpc_->call(kGameUtilities, 1, clientRequest(a), ans, err)) return false;
        if (ans.status) { err = "RealmListTicketRequest refused, status " + std::to_string(ans.status); return false; }
        const Attrs r = parseAttrs(ans.payload);
        const Variant* t = findAttr(r, "Param_RealmListTicket");
        if (!t) { err = "no realm list ticket (" + attrsText(r) + ")"; return false; }
        realmListTicket_.assign(t->s.begin(), t->s.end());
        say("Battle.net RPC: realm list ticket " + std::to_string(realmListTicket_.size()) + " bytes");
    }
    {   // the sub-regions ("1-1-0" ...)
        pb::W req;
        req.str(1, "Command_RealmListRequest_v1_b9");
        req.f32(5, kProgramWoW);
        if (!rpc_->call(kGameUtilities, 10, req, ans, err)) return false;
        if (ans.status) { err = "GetAllValuesForAttribute refused, status " + std::to_string(ans.status); return false; }
        subRegions_.clear();
        pb::each(ans.payload, [&](const pb::Field& f) {
            if (f.num == 1) { Variant v = parseVariant(f); if (v.t == VT::Str) subRegions_.push_back(v.s); }
        });
        std::string l;
        for (auto& s : subRegions_) l += " " + s;
        say("Battle.net RPC: sub-regions:" + (l.empty() ? std::string(" none") : l));
    }
    out.clear();
    for (const std::string& sub : subRegions_) {
        Attrs a = {{"Command_RealmListRequest_v1_b9", str(sub)}, {"Param_RealmListTicket", blob(realmListTicket_)}};
        if (!rpc_->call(kGameUtilities, 1, clientRequest(a), ans, err)) return false;
        if (ans.status) { say("RealmListRequest " + sub + ": status " + std::to_string(ans.status)); continue; }
        const Attrs r = parseAttrs(ans.payload);
        const Variant* list = findAttr(r, "Param_RealmList");
        const Variant* counts = findAttr(r, "Param_CharacterCountList");
        std::string json;
        if (!list || !unpackJsonBlob(Bytes(list->s.begin(), list->s.end()), json)) { say("realm list " + sub + ": not readable"); continue; }
        realmJson_ += json + "\n";
        Json j, jc;
        std::string jerr;
        if (!Json::parse(json, j, &jerr)) { say("realm list JSON: " + jerr); continue; }
        std::string cjson;
        if (counts && unpackJsonBlob(Bytes(counts->s.begin(), counts->s.end()), cjson)) Json::parse(cjson, jc);
        if (const Json* ups = j.get("updates"))
            for (const Json& u : ups->arr) {
                const Json* e = u.get("update");
                if (!e) continue;
                RealmInfo ri;
                // the real server (TrinityCore) puts the address inside the entry (update.wowRealmAddress); only a removed realm has it on the part
                ri.address = (uint32_t)e->numOr("wowRealmAddress", u.numOr("wowRealmAddress", 0));
                ri.subRegion = sub;
                ri.name = e->strOr("name", "?");
                ri.flags = (uint32_t)e->numOr("flags", 0);
                ri.population = (uint32_t)e->numOr("populationState", 0);
                ri.category = (uint32_t)e->numOr("cfgCategoriesID", 0);
                if (const Json* v = e->get("version")) {
                    ri.build = (uint32_t)v->numOr("versionBuild", 0);
                    char b[48];
                    snprintf(b, sizeof b, "%d.%d.%d.%u", (int)v->numOr("versionMajor", 0), (int)v->numOr("versionMinor", 0),
                             (int)v->numOr("versionRevision", 0), ri.build);
                    ri.versionText = b;
                }
                if (const Json* cs = jc.get("counts"))
                    for (const Json& c : cs->arr)
                        if ((uint32_t)c.numOr("wowRealmAddress", 0) == ri.address) ri.characters = (int)c.numOr("count", 0);
                out.push_back(ri);
            }
    }
    return true;
}

bool BnetLogin::joinRealm(const RealmInfo& realm, JoinInfo& out, std::string& err) {
    if (!rpc_) { err = "not logged on"; return false; }
    Rpc::Msg ans;
    Attrs a = {{"Command_RealmJoinRequest_v1_b9", str(realm.subRegion)}, {"Param_RealmAddress", uintv(realm.address)},
               {"Param_RealmListTicket", blob(realmListTicket_)}};
    if (!rpc_->call(kGameUtilities, 1, clientRequest(a), ans, err)) return false;
    if (ans.status) {
        err = "RealmJoinRequest refused, status " + std::to_string(ans.status) + " (a realm accepts only its own client build: the list shows it; try --build)";
        return false;
    }
    const Attrs r = parseAttrs(ans.payload);
    const Variant* t = findAttr(r, "Param_RealmJoinTicket");
    const Variant* addrs = findAttr(r, "Param_ServerAddresses");
    const Variant* sec = findAttr(r, "Param_JoinSecret");
    if (!t || !addrs || !sec || sec->s.size() != 32) { err = "incomplete join answer (" + attrsText(r) + ")"; return false; }
    out = JoinInfo();
    out.realmAddress = realm.address;
    out.joinTicket.assign(t->s.begin(), t->s.end());
    out.joinSecret.assign(sec->s.begin(), sec->s.end());
    out.clientSecret = clientSecret_;
    Json jt;
    if (Json::parse(std::string(t->s), jt)) out.gameAccount = jt.strOr("gameAccount", "");
    std::string json;
    Json j;
    if (!unpackJsonBlob(Bytes(addrs->s.begin(), addrs->s.end()), json) || !Json::parse(json, j)) { err = "server addresses not readable"; return false; }
    if (const Json* fams = j.get("families"))
        for (const Json& f : fams->arr)
            if (const Json* as = f.get("addresses"))
                for (const Json& ad : as->arr) out.addresses.emplace_back(ad.strOr("ip", ""), (int)ad.numOr("port", 0));
    if (out.addresses.empty()) { err = "no world server address in " + json; return false; }
    say("Battle.net RPC: join ticket for game account '" + out.gameAccount + "', world server " + out.addresses[0].first + ":" +
        std::to_string(out.addresses[0].second));
    return true;
}

}  // namespace wf
