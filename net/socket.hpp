// socket.hpp — minimal TCP client socket (Windows / POSIX / 3DS libctru).
// connect() is blocking (with the OS timeout); afterwards the socket is
// non-blocking so the game loop can poll it every frame.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace w3n {

bool netInit();       // WSAStartup / socInit (3DS); safe to call repeatedly
void netShutdown();
bool netRadioExclusive();   // 3DS: the Wi-Fi is ours alone (no StreetPass scans / SpotPass), see netInit

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    bool connect(const std::string& host, uint16_t port, std::string& err);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    // Sends everything (retries on would-block). false = connection lost.
    bool sendAll(const void* data, size_t len);
    // >0 bytes read, 0 nothing available, -1 closed / error.
    int recvSome(void* buf, size_t max);

private:
    long long fd_ = -1;
};

// "host:port" or "host" -> parts; returns false on a malformed port.
bool splitHostPort(const std::string& in, uint16_t defPort, std::string& host, uint16_t& port);
uint64_t nowMs();     // monotonic milliseconds
void sleepMs(int ms);

}  // namespace w3n
