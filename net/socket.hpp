// socket.hpp — minimal TCP client socket (Windows / POSIX / 3DS libctru).
// 6.55: beginConnect() starts the connect without waiting (the socket is non-blocking from the start) and pollConnect() is
// asked each frame - the login screen keeps drawing and can be cancelled while the server does not answer (a blocking connect
// waited for the system's own timeout: minutes on the 3DS, a black screen). connect() is the blocking form, for the tools.
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

    bool connect(const std::string& host, uint16_t port, std::string& err, int timeoutMs = 15000);
    // Resolves the name (a numeric address needs no lookup) and starts the connect. false: failed already (err).
    bool beginConnect(const std::string& host, uint16_t port, std::string& err);
    // 1 connected, 0 still waiting, -1 failed (err; the socket closed). The caller keeps the time.
    int pollConnect(std::string& err);
    bool connecting() const { return fd_ >= 0 && !connected_; }
    void close();
    bool isOpen() const { return fd_ >= 0; }
    // Sends everything (retries on would-block). false = connection lost.
    bool sendAll(const void* data, size_t len);
    // >0 bytes read, 0 nothing available, -1 closed / error.
    int recvSome(void* buf, size_t max);
    // 6.66: why the last close() happened ("recv errno 104", "peer closed", "send errno 32"), and how many transient errors were ridden out
    const std::string& closeReason() const { return reason_; }
    unsigned transientErrors() const { return transient_; }

private:
    long long fd_ = -1;
    bool connected_ = false;
    unsigned char addr_[16] = {};   // the sockaddr_in being connected to (6.55b: the 3DS asks connect() again for the state)
    std::string reason_;
    unsigned transient_ = 0;
    std::string target_;   // "host:port", for the messages
};

// "host:port" or "host" -> parts; returns false on a malformed port.
bool splitHostPort(const std::string& in, uint16_t defPort, std::string& host, uint16_t& port);
uint64_t nowMs();     // monotonic milliseconds
void sleepMs(int ms);

}  // namespace w3n
