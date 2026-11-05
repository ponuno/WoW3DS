// fnet.hpp — transport and crypto of the WoW Forever probe on top of mbedTLS 2.28 (extern/mbedtls, Apache-2.0):
// a TCP / TLS connection with timeouts, HTTPS POST, SHA-256 / SHA-512, HMAC-SHA512, AES-256-GCM with 12-byte tags.
#pragma once
#include <string>

#include "fbase.hpp"

namespace wf {

// One connection: plain TCP or TLS. TLS checks the server's certificate only with a CA file (the private servers sign
// their own); without one it is not checked (as the probe's purpose is to see the protocol, the log says so).
class Conn {
public:
    Conn();
    ~Conn();
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;
    bool open(const std::string& host, int port, bool tls, std::string& err, const std::string& caFile = "", int timeoutMs = 15000);
    void close();
    bool isOpen() const;
    bool writeAll(const void* p, size_t n, std::string& err);
    // Exactly n bytes within timeoutMs; false on timeout / close / error (err says which).
    bool readExact(void* p, size_t n, int timeoutMs, std::string& err);
    // Whatever arrives until the peer closes (HTTP with Connection: close), at most maxBytes.
    bool readToClose(Bytes& out, int timeoutMs, size_t maxBytes, std::string& err);
    // One line ending in '\n' (the world connection's greeting)
    bool readLine(std::string& line, int timeoutMs, size_t maxLen, std::string& err);
    std::string tlsInfo() const;   // "TLSv1.2 ECDHE-RSA-AES256-GCM-SHA384, certificate not checked"
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// POST body to https://host:port/path (or http when !tls); status and body of the answer.
bool httpPost(const std::string& host, int port, bool tls, const std::string& path, const std::string& body,
              int& status, std::string& response, std::string& err, const std::string& caFile = "");

namespace crypto {
void sha256(const void* p, size_t n, uint8_t out[32]);
void sha512(const void* p, size_t n, uint8_t out[64]);
void hmacSha512(const uint8_t* key, size_t keyLen, const Bytes& data, uint8_t out[64]);
void random(uint8_t* p, size_t n);
// AES-256-GCM, 12-byte IV, 12-byte tag, no additional data (in place)
bool gcmEncrypt(const uint8_t key[32], const uint8_t iv[12], uint8_t* data, size_t n, uint8_t tag[12]);
bool gcmDecrypt(const uint8_t key[32], const uint8_t iv[12], uint8_t* data, size_t n, const uint8_t tag[12]);
}  // namespace crypto

}  // namespace wf
