#include "socket.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define W3N_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
static int lastErr() { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define W3N_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
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

bool TcpSocket::connect(const std::string& host, uint16_t port, std::string& err) {
    close();
    if (!netInit()) { err = "network init failed (is Wi-Fi on?)"; return false; }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    char portStr[8];
    snprintf(portStr, sizeof portStr, "%u", port);
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0 || !res) { err = "cannot resolve " + host; return false; }
    sock_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((long long)s < 0
#if defined(_WIN32)
        || s == INVALID_SOCKET
#endif
    ) {
        freeaddrinfo(res);
        err = "socket() failed";
        return false;
    }
    if (::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        char buf[96];
        snprintf(buf, sizeof buf, "cannot connect to %s:%u (error %d)", host.c_str(), port, lastErr());
        err = buf;
        freeaddrinfo(res);
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
        return false;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    fd_ = (long long)s;
    return true;
}

void TcpSocket::close() {
    if (fd_ < 0) return;
#if defined(_WIN32)
    closesocket((sock_t)fd_);
#else
    ::close((sock_t)fd_);
#endif
    fd_ = -1;
}

bool TcpSocket::sendAll(const void* data, size_t len) {
    const char* p = (const char*)data;
    uint64_t start = nowMs();
    while (len > 0 && fd_ >= 0) {
        int n = (int)::send((sock_t)fd_, p, (int)len, 0);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && W3N_WOULDBLOCK(lastErr()) && nowMs() - start < 5000) { sleepMs(1); continue; }
        close();
        return false;
    }
    return fd_ >= 0;
}

int TcpSocket::recvSome(void* buf, size_t max) {
    if (fd_ < 0) return -1;
    int n = (int)::recv((sock_t)fd_, (char*)buf, (int)max, 0);
    if (n > 0) return n;
    if (n < 0 && W3N_WOULDBLOCK(lastErr())) return 0;
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
