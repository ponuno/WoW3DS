#include "socket.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define W3N_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#define W3N_TRANSIENT(e) ((e) == WSAEINTR || (e) == WSAENOBUFS)
static int lastErr() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define W3N_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define W3N_TRANSIENT(e) ((e) == EINTR || (e) == ENOBUFS || (e) == ENOMEM)   // 6.66: the 3DS socket service runs out of buffers under load
static int lastErr() { return errno; }
#endif

#if defined(__3DS__)
#include <3ds.h>
#include <malloc.h>
#endif

namespace w3n {


static bool g_netUp = false;
#if defined(__3DS__)
static u32* g_socBuf = nullptr;
// 5.35d: Wi-Fi only for us while the game runs. The system otherwise keeps scanning for StreetPass and lets SpotPass /
// friends / eShop daemons use the radio in the background: latency spikes of tens to hundreds of ms. As ftpd does
// (exclusive infrastructure state, locked), plus the background daemons suspended; all undone in netShutdown.
static bool g_ndmUp = false, g_ndmLocked = false, g_ndmSuspended = false;
#endif

bool netRadioExclusive() {
#if defined(__3DS__)
    return g_ndmLocked;
#else
    return false;
#endif
}

bool netInit() {
    if (g_netUp) return true;
#if defined(_WIN32)
    WSADATA wsa;
    g_netUp = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#elif defined(__3DS__)
    const u32 kSocBufSize = 0x100000;   // 1 MB, must be 0x1000-aligned
    g_socBuf = (u32*)memalign(0x1000, kSocBufSize);
    if (g_socBuf && R_SUCCEEDED(socInit(g_socBuf, kSocBufSize))) g_netUp = true;
    else { free(g_socBuf); g_socBuf = nullptr; }
    if (g_netUp && R_SUCCEEDED(ndmuInit())) {
        g_ndmUp = true;
        if (R_SUCCEEDED(NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE))) {
            if (R_SUCCEEDED(NDMU_LockState())) g_ndmLocked = true;
            else NDMU_LeaveExclusiveState();
        }
        g_ndmSuspended = R_SUCCEEDED(NDMU_SuspendDaemons(NDM_DAEMON_MASK_ALL));
    }
#else
    g_netUp = true;
#endif
    return g_netUp;
}

void netShutdown() {
    if (!g_netUp) return;
#if defined(_WIN32)
    WSACleanup();
#elif defined(__3DS__)
    if (g_ndmUp) {
        if (g_ndmSuspended) NDMU_ResumeDaemons(NDM_DAEMON_MASK_ALL);
        if (g_ndmLocked) { NDMU_UnlockState(); NDMU_LeaveExclusiveState(); }
        ndmuExit();
        g_ndmUp = g_ndmLocked = g_ndmSuspended = false;
    }
    socExit();
    free(g_socBuf);
    g_socBuf = nullptr;
#endif
    g_netUp = false;
}

static void closeSock(sock_t s) {
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

bool TcpSocket::beginConnect(const std::string& host, uint16_t port, std::string& err) {
    close();
    if (!netInit()) { err = "network init failed (is Wi-Fi on?)"; return false; }
    char portStr[8];
    snprintf(portStr, sizeof portStr, "%u", port);
    target_ = host + ":" + portStr;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {   // a name: the lookup (a numeric address needs none)
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
        if (gai != 0 || !res) { err = "cannot resolve " + host; if (res) freeaddrinfo(res); return false; }
        std::memcpy(&sa, res->ai_addr, std::min(sizeof sa, (size_t)res->ai_addrlen));
        freeaddrinfo(res);
    }
    sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((long long)s < 0
#if defined(_WIN32)
        || s == INVALID_SOCKET
#endif
    ) {
        err = "socket() failed";
        return false;
    }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    static_assert(sizeof sa <= sizeof addr_, "sockaddr_in");
    std::memcpy(addr_, &sa, sizeof sa);
    if (::connect(s, (const sockaddr*)&sa, (int)sizeof sa) == 0) {
        connected_ = true;
    } else {
        const int e = lastErr();
#if defined(_WIN32)
        const bool pending = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
        const bool pending = e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN || e == EALREADY;
#endif
        if (!pending) {
            char buf[96];
            snprintf(buf, sizeof buf, "cannot connect to %s (error %d)", target_.c_str(), e);
            err = buf;
            closeSock(s);
            return false;
        }
        connected_ = false;
    }
    fd_ = (long long)s;
    if (connected_) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    }
    return true;
}

int TcpSocket::pollConnect(std::string& err) {
    if (fd_ < 0) { if (err.empty()) err = "not connected"; return -1; }
    if (connected_) return 1;
    const sock_t s = (sock_t)fd_;
    int soErr = 0;
#if defined(__3DS__) || defined(W3N_CONNECT_RETRY)   // (W3N_CONNECT_RETRY: the same path on the PC, for connect_test)
    // 6.55b: the 3DS's poll() says "writable" while the connect is still under way and SO_ERROR then holds the system's own
    // code for "in progress" (-26: the first build failed every login with "error -26"). As ftpd does: connect() again tells
    // the state - EISCONN connected, EINPROGRESS / EALREADY still waiting, anything else the error.
    if (::connect(s, (const sockaddr*)addr_, (int)sizeof(sockaddr_in)) == 0) soErr = 0;
    else {
        const int e = errno;
        if (e == EISCONN) soErr = 0;
        else if (e == EINPROGRESS || e == EALREADY || e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return 0;
        else soErr = e;
    }
    if (soErr != 0 && soErr != ECONNREFUSED && soErr != ETIMEDOUT && soErr != EHOSTUNREACH && soErr != ENETUNREACH &&
        soErr != ENETDOWN && soErr != ECONNRESET) {
        // an answer this code does not expect from the system: once more the way 0.3.x connected (blocking), not a failed login
        const int first = soErr;
        close();
        sock_t b = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if ((long long)b >= 0 && ::connect(b, (const sockaddr*)addr_, (int)sizeof(sockaddr_in)) == 0) {
            fcntl(b, F_SETFL, fcntl(b, F_GETFL, 0) | O_NONBLOCK);
            fd_ = (long long)b;
            connected_ = true;
            int one = 1;
            setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            return 1;
        }
        const int e2 = errno;
        if ((long long)b >= 0) closeSock(b);
        char buf[128];
        snprintf(buf, sizeof buf, "cannot connect to %s (error %d, then %d)", target_.c_str(), first, e2);
        err = buf;
        return -1;
    }
#else
    bool ready = false, failed = false;
#if defined(_WIN32)
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv{0, 0};
    const int n = ::select(0, nullptr, &wr, &ex, &tv);
    if (n > 0) { ready = FD_ISSET(s, &wr) != 0; failed = FD_ISSET(s, &ex) != 0; }
    else if (n < 0) failed = true;
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLOUT;
    const int n = ::poll(&p, 1, 0);
    if (n > 0) { ready = (p.revents & POLLOUT) != 0; failed = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0; }
    else if (n < 0) failed = true;
#endif
    if (!ready && !failed) return 0;
    socklen_t len = sizeof soErr;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &len) != 0) soErr = failed ? -1 : 0;
#if defined(_WIN32)
    if (soErr == WSAEWOULDBLOCK || soErr == WSAEINPROGRESS || soErr == WSAEALREADY) return 0;   // (still under way)
#else
    if (soErr == EINPROGRESS || soErr == EALREADY) return 0;
#endif
    if (failed && soErr == 0) soErr = -1;
#endif
    if (soErr != 0) {
        char buf[96];
        snprintf(buf, sizeof buf, "cannot connect to %s (error %d)", target_.c_str(), soErr);
        err = buf;
        close();
        return -1;
    }
    connected_ = true;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    return 1;
}

bool TcpSocket::connect(const std::string& host, uint16_t port, std::string& err, int timeoutMs) {
    if (!beginConnect(host, port, err)) return false;
    const uint64_t until = nowMs() + (uint64_t)timeoutMs;
    for (;;) {
        const int r = pollConnect(err);
        if (r > 0) return true;
        if (r < 0) return false;
        if (nowMs() > until) {
            err = "cannot connect to " + target_ + " (no answer)";
            close();
            return false;
        }
        sleepMs(5);
    }
}

void TcpSocket::close() {
    if (fd_ < 0) return;
#if defined(_WIN32)
    closesocket((sock_t)fd_);
#else
    ::close((sock_t)fd_);
#endif
    fd_ = -1;
    connected_ = false;
}

bool TcpSocket::sendAll(const void* data, size_t len) {
    const char* p = (const char*)data;
    uint64_t start = nowMs();
    while (len > 0 && fd_ >= 0) {
        int n = (int)::send((sock_t)fd_, p, (int)len, 0);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        const int e = lastErr();
        if (n < 0 && (W3N_WOULDBLOCK(e) || W3N_TRANSIENT(e)) && nowMs() - start < 5000) { if (!W3N_WOULDBLOCK(e)) ++transient_; sleepMs(1); continue; }
        reason_ = n < 0 ? "send error " + std::to_string(e) + (nowMs() - start >= 5000 ? " (blocked 5 s)" : "") : "send returned 0";
        close();
        return false;
    }
    return fd_ >= 0;
}

int TcpSocket::recvSome(void* buf, size_t max) {
    if (fd_ < 0) return -1;
    int n = (int)::recv((sock_t)fd_, (char*)buf, (int)max, 0);
    if (n > 0) return n;
    const int e = n < 0 ? lastErr() : 0;
    if (n < 0 && W3N_WOULDBLOCK(e)) return 0;
    if (n < 0 && W3N_TRANSIENT(e)) { ++transient_; return 0; }   // 6.66: try again next frame
    reason_ = n < 0 ? "receive error " + std::to_string(e) : "the server closed the connection";
    close();   // n == 0: orderly shutdown by the peer
    return -1;
}

bool splitHostPort(const std::string& in, uint16_t defPort, std::string& host, uint16_t& port) {
    size_t c = in.rfind(':');
    if (c == std::string::npos) { host = in; port = defPort; return !host.empty(); }
    host = in.substr(0, c);
    long p = strtol(in.c_str() + c + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    port = (uint16_t)p;
    return !host.empty();
}

uint64_t nowMs() {
#if defined(__3DS__)
    return osGetTime();
#elif defined(_WIN32)
    return GetTickCount64();
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void sleepMs(int ms) {
#if defined(__3DS__)
    svcSleepThread((s64)ms * 1000000);
#elif defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

}  // namespace w3n
