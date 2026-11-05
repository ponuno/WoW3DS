// 6.55: the non-blocking connect (socket.cpp): an open port connects, a closed one fails at once, an address that never answers
// keeps pollConnect at 0 (the caller's timeout decides) - and none of the calls waits.
//   connect_test <open port on 127.0.0.1> <closed port> [an address that does not answer, e.g. 10.255.255.1]
#include "../socket.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace w3n;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: connect_test <open port> <closed port> [silent address]"); return 2; }
    std::string err;
    {   // open
        TcpSocket s;
        uint64_t t0 = nowMs();
        CHECK(s.beginConnect("127.0.0.1", (uint16_t)std::atoi(argv[1]), err), "begin: %s", err.c_str());
        CHECK(nowMs() - t0 < 200, "beginConnect waited %llu ms", (unsigned long long)(nowMs() - t0));
        int r = 0;
        for (int k = 0; k < 2000 && r == 0; ++k) { r = s.pollConnect(err); if (!r) sleepMs(1); }
        CHECK(r == 1 && s.isOpen() && !s.connecting(), "open port: %d %s", r, err.c_str());
        std::printf("open port: connected in %llu ms\n", (unsigned long long)(nowMs() - t0));
    }
    {   // closed: refused
        TcpSocket s;
        err.clear();
        uint64_t t0 = nowMs();
        int r = s.beginConnect("127.0.0.1", (uint16_t)std::atoi(argv[2]), err) ? 0 : -1;
        for (int k = 0; k < 2000 && r == 0; ++k) { r = s.pollConnect(err); if (!r) sleepMs(1); }
        CHECK(r == -1 && !s.isOpen() && !err.empty(), "closed port: %d", r);
        std::printf("closed port: failed in %llu ms: %s\n", (unsigned long long)(nowMs() - t0), err.c_str());
    }
    {   // a name that cannot be resolved
        TcpSocket s;
        err.clear();
        CHECK(!s.beginConnect("no-such-host.invalid", 3724, err) && !err.empty(), "bad name accepted");
        std::printf("bad name: %s\n", err.c_str());
    }
    if (argc > 3) {   // silent: stays pending, each poll returns at once; the blocking form gives up at its timeout
        TcpSocket s;
        err.clear();
        if (s.beginConnect(argv[3], 3724, err)) {
            uint64_t worst = 0;
            int r = 0;
            const uint64_t t0 = nowMs();
            while (nowMs() - t0 < 1500 && r == 0) {
                const uint64_t a = nowMs();
                r = s.pollConnect(err);
                worst = std::max<uint64_t>(worst, nowMs() - a);
                sleepMs(10);
            }
            CHECK(worst < 50, "a poll waited %llu ms", (unsigned long long)worst);
            std::printf("silent address: after 1.5 s poll says %d (%s), the longest poll %llu ms\n", r, err.c_str(), (unsigned long long)worst);
        } else std::printf("silent address: refused at once here (%s) - nothing to wait for on this machine\n", err.c_str());
        TcpSocket b;
        const uint64_t t0 = nowMs();
        const bool ok = b.connect(argv[3], 3724, err, 800);
        std::printf("blocking form, 0.8 s: %s after %llu ms (%s)\n", ok ? "connected" : "gave up", (unsigned long long)(nowMs() - t0), err.c_str());
        CHECK(ok || nowMs() - t0 < 1500, "the timeout was not kept");
    }
    std::printf("CONNECT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
