// The fatal alert code is read from a private field of the TLS context (for the log only).
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "trojan.h"

#include "net_proxy.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
#include <string>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "psa/crypto.h"

namespace valet::trojan {
namespace {
constexpr char TAG[] = "Trojan";

constexpr size_t kHashHex = 56;                                   // hex of SHA-224
constexpr size_t kMaxHeader = kHashHex + 2 + 1 + 1 + 1 + 255 + 2 + 2;
constexpr size_t kChunk = 4096;                                   // one TLS record out (MBEDTLS_SSL_OUT_CONTENT_LEN)
constexpr uint32_t kRelayStack = 6144;

// The relay: between the caller's loopback socket and the TLS connection to the server. Lives in PSRAM.
struct Relay {
    int remote = -1, local = -1;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    // Sent in front of the caller's first data, in one record (servers read the header from the first record).
    uint8_t header[kMaxHeader] = {};
    size_t header_size = 0;
    uint8_t up[kMaxHeader + kChunk];
    uint8_t down[kChunk];
    Relay() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
    }
    ~Relay() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        std::memset(header, 0, sizeof(header));
        if (remote >= 0) close(remote);
        if (local >= 0) close(local);
    }
};

Relay* NewRelay() {
    void* memory = heap_caps_malloc(sizeof(Relay), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? new (memory) Relay() : nullptr;
}
void FreeRelay(Relay* relay) {
    relay->~Relay();
    heap_caps_free(relay);
}

// TLS reads and writes on the server socket. A timeout (during the handshake the socket has one)
// is an error, not "try again": the handshake must not spin on it.
int Send(void* context, const unsigned char* data, size_t size) {
    const int n = send(*static_cast<int*>(context), data, size, 0);
    return n >= 0 ? n : MBEDTLS_ERR_NET_SEND_FAILED;
}
int Receive(void* context, unsigned char* data, size_t size) {
    const int n = recv(*static_cast<int*>(context), data, size, 0);
    if (n >= 0) return n;   // 0: the server closed
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

bool SendAll(int fd, const uint8_t* data, size_t size) {
    while (size > 0) {
        const int n = send(fd, data, size, 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}
bool WriteAll(mbedtls_ssl_context* ssl, const uint8_t* data, size_t size) {
    while (size > 0) {
        const int n = mbedtls_ssl_write(ssl, data, size);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// Caller -> server.
bool Upstream(Relay& r) {
    uint8_t* start = r.up + r.header_size;
    const int n = recv(r.local, start, kChunk, 0);
    if (n <= 0) return false;
    size_t size = static_cast<size_t>(n);
    if (r.header_size) {
        std::memcpy(r.up, r.header, r.header_size);
        size += r.header_size;
        std::memset(r.header, 0, sizeof(r.header));
        r.header_size = 0;
    }
    return WriteAll(&r.ssl, r.up, size);
}

// Server -> caller.
bool Downstream(Relay& r) {
    const int n = mbedtls_ssl_read(&r.ssl, r.down, sizeof(r.down));
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return true;
    if (n <= 0) return false;   // closed (0 or close-notify) or broken
    return SendAll(r.local, r.down, static_cast<size_t>(n));
}

void RelayTask(void* arg) {
    auto* relay = static_cast<Relay*>(arg);
    const int top = std::max(relay->remote, relay->local) + 1;
    while (true) {
        // Data TLS has already taken off the socket does not make the socket readable again.
        bool from_server = mbedtls_ssl_get_bytes_avail(&relay->ssl) > 0;
        bool from_caller = false;
        if (!from_server) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(relay->remote, &fds);
            FD_SET(relay->local, &fds);
            if (select(top, &fds, nullptr, nullptr, nullptr) <= 0) break;
            from_server = FD_ISSET(relay->remote, &fds);
            from_caller = FD_ISSET(relay->local, &fds);
        }
        if (from_caller && !Upstream(*relay)) break;
        if (from_server && !Downstream(*relay)) break;
    }
    mbedtls_ssl_close_notify(&relay->ssl);
    shutdown(relay->local, SHUT_RDWR);
    shutdown(relay->remote, SHUT_RDWR);
    FreeRelay(relay);
    vTaskDeleteWithCaps(nullptr);
}

// A connected loopback pair: `outer` is handed to the caller, `inner` belongs to the relay.
bool LoopbackPair(int* outer, int* inner) {
    *outer = *inner = -1;
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    bool ok = bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0 &&
              getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    if (ok) {
        *outer = socket(AF_INET, SOCK_STREAM, 0);
        ok = *outer >= 0 && connect(*outer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    }
    if (ok) {
        *inner = accept(listener, nullptr, nullptr);
        ok = *inner >= 0;
    }
    close(listener);
    if (!ok) {
        if (*outer >= 0) close(*outer);
        if (*inner >= 0) close(*inner);
        *outer = *inner = -1;
    }
    return ok;
}

// hex(SHA-224(password)) CRLF 01 address port CRLF.
bool BuildHeader(Relay& r, const std::string& password, const std::string& host, int port) {
    uint8_t hash[28];
    size_t hash_size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_224, reinterpret_cast<const uint8_t*>(password.data()), password.size(),
                         hash, sizeof(hash), &hash_size) != PSA_SUCCESS || hash_size != sizeof(hash)) return false;
    static const char kHex[] = "0123456789abcdef";
    uint8_t* h = r.header;
    size_t size = 0;
    for (uint8_t byte : hash) {
        h[size++] = kHex[byte >> 4];
        h[size++] = kHex[byte & 15];
    }
    std::memset(hash, 0, sizeof(hash));
    h[size++] = '\r';
    h[size++] = '\n';
    h[size++] = 1;   // CONNECT
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        h[size++] = 1;
        std::memcpy(h + size, &literal.s_addr, 4);
        size += 4;
    } else {
        h[size++] = 3;
        h[size++] = static_cast<uint8_t>(host.size());
        std::memcpy(h + size, host.data(), host.size());
        size += host.size();
    }
    h[size++] = static_cast<uint8_t>((port >> 8) & 0xFF);
    h[size++] = static_cast<uint8_t>(port & 0xFF);
    h[size++] = '\r';
    h[size++] = '\n';
    r.header_size = size;
    return true;
}

int Open(int remote, const net_proxy::Config& proxy, const std::string& host, int port, int* error) {
    if (host.empty() || host.size() > 255) {
        close(remote);
        *error = net_proxy::kErrProxyProtocol;
        return -1;
    }
    if (proxy.password.empty()) {
        close(remote);
        *error = net_proxy::kErrProxyAuth;
        return -1;
    }
    Relay* relay = NewRelay();
    if (!relay) { close(remote); *error = ENOMEM; return -1; }
    relay->remote = remote;
    psa_crypto_init();   // TLS and the hash need it; a no-op once done
    if (!BuildHeader(*relay, proxy.password, host, port)) {
        FreeRelay(relay);
        *error = net_proxy::kErrProxyProtocol;
        return -1;
    }

    // TLS with the server. The socket still has the time left from net_proxy::Open as its limit.
    const std::string& name = proxy.sni.empty() ? proxy.host : proxy.sni;
    int ret = mbedtls_ssl_config_defaults(&relay->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret == 0) {
        if (proxy.insecure) {
            mbedtls_ssl_conf_authmode(&relay->conf, MBEDTLS_SSL_VERIFY_NONE);
        } else {
            mbedtls_ssl_conf_authmode(&relay->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            esp_crt_bundle_attach(&relay->conf);
        }
        ret = mbedtls_ssl_setup(&relay->ssl, &relay->conf);
    }
    if (ret == 0) ret = mbedtls_ssl_set_hostname(&relay->ssl, name.c_str());   // SNI, and the name the certificate must carry
    if (ret == 0) {
        mbedtls_ssl_set_bio(&relay->ssl, &relay->remote, Send, Receive, nullptr);
        while ((ret = mbedtls_ssl_handshake(&relay->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {}
    }
    if (ret != 0) {
        // A certificate the bundle does not trust comes back from its verify callback as X509_FATAL_ERROR.
        const bool certificate = !proxy.insecure && (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || ret == MBEDTLS_ERR_X509_FATAL_ERROR);
        // The server's name stays out of the log.
        // A fatal alert: its code says why (40 handshake failure, 70 protocol version, 112 unrecognised name).
        const unsigned char* alert = ret == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE ? relay->ssl.MBEDTLS_PRIVATE(in_msg) : nullptr;
        ESP_LOGW(TAG, "TLS with the server failed: -0x%04X%s alert=%d", static_cast<unsigned>(-ret),
                 certificate ? " (certificate not valid for the name)" : "", alert ? alert[1] : -1);
        FreeRelay(relay);
        *error = certificate ? net_proxy::kErrProxyCertificate
               : ret == MBEDTLS_ERR_SSL_TIMEOUT ? net_proxy::kErrProxyNoAnswer : net_proxy::kErrProxyTls;
        return -1;
    }
    timeval none{};
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
    int outer = -1;
    if (!LoopbackPair(&outer, &relay->local)) {
        FreeRelay(relay);
        *error = errno ? errno : EMFILE;
        return -1;
    }
    // The relay only moves bytes (no flash or NVS), so its stack may live in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(RelayTask, "trojan_relay", kRelayStack, relay, tskIDLE_PRIORITY + 5, nullptr,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        FreeRelay(relay);
        close(outer);
        *error = ENOMEM;
        return -1;
    }
    return outer;
}
}  // namespace

void Install() { net_proxy::SetOpener(net_proxy::Kind::kTrojan, &Open); }

}  // namespace valet::trojan
