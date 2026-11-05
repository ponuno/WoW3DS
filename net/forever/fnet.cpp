// fnet.cpp — see fnet.hpp
#include "fnet.hpp"

#include <chrono>
#include <cstdio>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace wf {

namespace {
std::string mbedErr(int rc) {
    char b[160];
    mbedtls_strerror(rc, b, sizeof b);
    char out[200];
    snprintf(out, sizeof out, "%s (-0x%04x)", b, (unsigned)-rc);
    return out;
}
struct Rng {
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    bool ok = false;
    Rng() {
        mbedtls_entropy_init(&ent);
        mbedtls_ctr_drbg_init(&drbg);
        const char* pers = "w3dfprobe";
        ok = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char*)pers, strlen(pers)) == 0;
    }
    ~Rng() { mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&ent); }
};
Rng& rng() { static Rng r; return r; }
uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

struct Conn::Impl {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    bool tls = false, open = false, verified = false;
    Impl() {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&ca);
    }
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&ca);
        mbedtls_net_free(&net);
    }
    // >0 bytes, 0 timeout, <0 error / closed
    int read(uint8_t* p, size_t n, int timeoutMs) {
        if (tls) {
            mbedtls_ssl_conf_read_timeout(&conf, (uint32_t)(timeoutMs > 0 ? timeoutMs : 1));
            int rc = mbedtls_ssl_read(&ssl, p, n);
            if (rc == MBEDTLS_ERR_SSL_TIMEOUT || rc == MBEDTLS_ERR_SSL_WANT_READ) return 0;
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
            return rc;
        }
        int rc = mbedtls_net_recv_timeout(&net, p, n, (uint32_t)(timeoutMs > 0 ? timeoutMs : 1));
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT || rc == MBEDTLS_ERR_SSL_WANT_READ) return 0;
        if (rc == 0) return -1;
        return rc;
    }
};

Conn::Conn() : d_(new Impl) {}
Conn::~Conn() { close(); }

bool Conn::open(const std::string& host, int port, bool tls, std::string& err, const std::string& caFile, int timeoutMs) {
    close();
    d_.reset(new Impl);
    (void)timeoutMs;   // (mbedtls_net_connect blocks for the system's connect timeout)
    const std::string portStr = std::to_string(port);
    int rc = mbedtls_net_connect(&d_->net, host.c_str(), portStr.c_str(), MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) { err = "connect " + host + ":" + portStr + ": " + mbedErr(rc); return false; }
    d_->open = true;
    d_->tls = tls;
    if (!tls) return true;
    if (!rng().ok) { err = "no random source"; return false; }
    if ((rc = mbedtls_ssl_config_defaults(&d_->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        err = "TLS config: " + mbedErr(rc);
        return false;
    }
    if (!caFile.empty()) {
        if ((rc = mbedtls_x509_crt_parse_file(&d_->ca, caFile.c_str())) != 0) { err = "CA file " + caFile + ": " + mbedErr(rc); return false; }
        mbedtls_ssl_conf_ca_chain(&d_->conf, &d_->ca, nullptr);
        mbedtls_ssl_conf_authmode(&d_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        d_->verified = true;
    } else {
        mbedtls_ssl_conf_authmode(&d_->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&d_->conf, mbedtls_ctr_drbg_random, &rng().drbg);
    if ((rc = mbedtls_ssl_setup(&d_->ssl, &d_->conf)) != 0) { err = "TLS setup: " + mbedErr(rc); return false; }
    mbedtls_ssl_set_hostname(&d_->ssl, host.c_str());
    mbedtls_ssl_set_bio(&d_->ssl, &d_->net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    mbedtls_ssl_conf_read_timeout(&d_->conf, 15000);
    while ((rc = mbedtls_ssl_handshake(&d_->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        err = "TLS handshake with " + host + ":" + portStr + ": " + mbedErr(rc);
        if (d_->verified) err += " (the certificate is checked against " + caFile + "; without --ca it is not checked)";
        return false;
    }
    return true;
}

void Conn::close() {
    if (d_ && d_->open) {
        if (d_->tls) mbedtls_ssl_close_notify(&d_->ssl);
        mbedtls_net_free(&d_->net);
        d_->open = false;
    }
}

bool Conn::isOpen() const { return d_ && d_->open; }

std::string Conn::tlsInfo() const {
    if (!d_ || !d_->tls) return "plain TCP";
    std::string s = mbedtls_ssl_get_version(&d_->ssl);
    s += " ";
    s += mbedtls_ssl_get_ciphersuite(&d_->ssl);
    s += d_->verified ? ", certificate checked" : ", certificate NOT checked (no --ca)";
    return s;
}

bool Conn::writeAll(const void* p, size_t n, std::string& err) {
    const uint8_t* c = (const uint8_t*)p;
    while (n > 0) {
        int rc = d_->tls ? mbedtls_ssl_write(&d_->ssl, c, n) : mbedtls_net_send(&d_->net, c, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_WANT_READ) continue;
        if (rc <= 0) { err = "send: " + mbedErr(rc); return false; }
        c += rc;
        n -= (size_t)rc;
    }
    return true;
}

bool Conn::readExact(void* p, size_t n, int timeoutMs, std::string& err) {
    uint8_t* c = (uint8_t*)p;
    const uint64_t until = nowMs() + (uint64_t)timeoutMs;
    while (n > 0) {
        const uint64_t now = nowMs();
        if (now >= until) { err = "timeout"; return false; }
        int rc = d_->read(c, n, (int)(until - now));
        if (rc == 0) continue;
        if (rc < 0) { err = rc == -1 ? "closed by the server" : "receive: " + mbedErr(rc); return false; }
        c += rc;
        n -= (size_t)rc;
    }
    return true;
}

bool Conn::readToClose(Bytes& out, int timeoutMs, size_t maxBytes, std::string& err) {
    const uint64_t until = nowMs() + (uint64_t)timeoutMs;
    uint8_t buf[4096];
    while (out.size() < maxBytes) {
        const uint64_t now = nowMs();
        if (now >= until) { err = "timeout"; return !out.empty(); }
        int rc = d_->read(buf, sizeof buf, (int)(until - now));
        if (rc == 0) continue;
        if (rc < 0) return true;   // closed: done
        out.insert(out.end(), buf, buf + rc);
    }
    return true;
}

bool Conn::readLine(std::string& line, int timeoutMs, size_t maxLen, std::string& err) {
    line.clear();
    while (line.size() < maxLen) {
        char c;
        if (!readExact(&c, 1, timeoutMs, err)) return false;
        line += c;
        if (c == '\n') return true;
    }
    err = "line too long";
    return false;
}

bool httpPost(const std::string& host, int port, bool tls, const std::string& path, const std::string& body,
              int& status, std::string& response, std::string& err, const std::string& caFile) {
    Conn c;
    if (!c.open(host, port, tls, err, caFile)) return false;
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nUser-Agent: Battle.net/1.0\r\nAccept: application/json\r\nContent-Type: application/json;charset=utf-8\r\nContent-Length: " +
                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (!c.writeAll(req.data(), req.size(), err)) return false;
    Bytes raw;
    if (!c.readToClose(raw, 15000, 1 << 20, err)) return false;
    std::string s((const char*)raw.data(), raw.size());
    const size_t sp = s.find(' ');
    status = sp != std::string::npos ? atoi(s.c_str() + sp + 1) : 0;
    const size_t hdrEnd = s.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) { err = "no HTTP header in the answer"; return false; }
    std::string headers = s.substr(0, hdrEnd);
    std::string data = s.substr(hdrEnd + 4);
    for (char& ch : headers) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {   // chunks -> one body
        std::string out;
        size_t i = 0;
        while (i < data.size()) {
            const size_t eol = data.find("\r\n", i);
            if (eol == std::string::npos) break;
            const size_t n = strtoul(data.substr(i, eol - i).c_str(), nullptr, 16);
            if (n == 0) break;
            out += data.substr(eol + 2, n);
            i = eol + 2 + n + 2;
        }
        data = out;
    }
    response = data;
    return true;
}

namespace crypto {
void sha256(const void* p, size_t n, uint8_t out[32]) { mbedtls_sha256_ret((const unsigned char*)p, n, out, 0); }
void sha512(const void* p, size_t n, uint8_t out[64]) { mbedtls_sha512_ret((const unsigned char*)p, n, out, 0); }
void hmacSha512(const uint8_t* key, size_t keyLen, const Bytes& data, uint8_t out[64]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), key, keyLen, data.data(), data.size(), out);
}
void random(uint8_t* p, size_t n) { mbedtls_ctr_drbg_random(&rng().drbg, p, n); }
bool gcmEncrypt(const uint8_t key[32], const uint8_t iv[12], uint8_t* data, size_t n, uint8_t tag[12]) {
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12, nullptr, 0, data, data, 12, tag) == 0;
    mbedtls_gcm_free(&g);
    return ok;
}
bool gcmDecrypt(const uint8_t key[32], const uint8_t iv[12], uint8_t* data, size_t n, const uint8_t tag[12]) {
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&g, n, iv, 12, nullptr, 0, tag, 12, data, data) == 0;
    mbedtls_gcm_free(&g);
    return ok;
}
}  // namespace crypto

}  // namespace wf
