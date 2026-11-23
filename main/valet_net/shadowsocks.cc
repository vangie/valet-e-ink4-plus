// mbedtls 4 keeps the block-cipher API private; AES and GCM are built in for TLS already.
// Defined before any mbedtls / PSA header, which may pull the private headers in first.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "shadowsocks.h"

#include "net_proxy.h"
#include "valet_crypto/monocypher.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_md5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "psa/crypto.h"
#include "mbedtls/private/aes.h"
#include "mbedtls/private/gcm.h"

namespace valet::ss {
namespace {
constexpr char TAG[] = "Shadowsocks";

enum class Kind : uint8_t { kCfb, kCtr, kChacha, kGcm, kChachaPoly, kXChachaPoly };
struct Method {
    const char* name;
    Kind kind;
    uint8_t key;   // key length
    uint8_t iv;    // stream: IV length; AEAD: salt length
};
constexpr Method kMethods[] = {
    {"aes-128-gcm", Kind::kGcm, 16, 16},
    {"aes-192-gcm", Kind::kGcm, 24, 24},
    {"aes-256-gcm", Kind::kGcm, 32, 32},
    {"chacha20-ietf-poly1305", Kind::kChachaPoly, 32, 32},
    {"xchacha20-ietf-poly1305", Kind::kXChachaPoly, 32, 32},
    {"aes-128-cfb", Kind::kCfb, 16, 16},
    {"aes-192-cfb", Kind::kCfb, 24, 16},
    {"aes-256-cfb", Kind::kCfb, 32, 16},
    {"aes-128-ctr", Kind::kCtr, 16, 16},
    {"aes-192-ctr", Kind::kCtr, 24, 16},
    {"aes-256-ctr", Kind::kCtr, 32, 16},
    {"chacha20-ietf", Kind::kChacha, 32, 12},
};
const Method* Find(const std::string& name) {
    const std::string wanted = name.empty() ? kDefaultMethod : name;
    for (const auto& m : kMethods) if (wanted == m.name) return &m;
    return nullptr;
}
bool Aead(const Method& m) { return m.kind == Kind::kGcm || m.kind == Kind::kChachaPoly || m.kind == Kind::kXChachaPoly; }

constexpr size_t kTag = 16;
constexpr size_t kMaxPayload = 0x3FFF;        // AEAD chunk limit (SIP004)
constexpr size_t kChunk = 1460;               // what one read from the caller's side takes
constexpr size_t kMaxSalt = 32, kMaxAddress = 1 + 1 + 255 + 2;
constexpr uint32_t kRelayStack = 4096;

// EVP_BytesToKey with MD5: the password becomes the master key.
bool MasterKey(const std::string& password, uint8_t* key, size_t size) {
    if (password.empty() || password.size() > 128) return false;
    uint8_t block[16];
    for (size_t filled = 0; filled < size; filled += 16) {
        md5_context_t md5;
        esp_rom_md5_init(&md5);
        if (filled) esp_rom_md5_update(&md5, block, sizeof(block));
        esp_rom_md5_update(&md5, password.data(), password.size());
        esp_rom_md5_final(block, &md5);
        std::memcpy(key + filled, block, std::min<size_t>(16, size - filled));
    }
    std::memset(block, 0, sizeof(block));
    return true;
}

// HKDF-SHA1(master key, salt, "ss-subkey"): each connection direction's AEAD key. PSA's HKDF is
// already linked for TLS 1.3.
bool Subkey(const uint8_t* master, size_t size, const uint8_t* salt, size_t salt_size, uint8_t* out) {
    static const uint8_t kInfo[] = {'s', 's', '-', 's', 'u', 'b', 'k', 'e', 'y'};
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_1));
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_size);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, master, size);
    if (status == PSA_SUCCESS) status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, kInfo, sizeof(kInfo));
    if (status == PSA_SUCCESS) status = psa_key_derivation_output_bytes(&op, out, size);
    psa_key_derivation_abort(&op);
    if (status != PSA_SUCCESS) ESP_LOGW(TAG, "HKDF-SHA1 failed (code %d)", int(status));
    return status == PSA_SUCCESS;
}

// One direction of a connection.
class Cipher {
public:
    Cipher() { mbedtls_aes_init(&aes_); mbedtls_gcm_init(&gcm_); }
    ~Cipher() {
        mbedtls_aes_free(&aes_);
        mbedtls_gcm_free(&gcm_);
        std::memset(key_, 0, sizeof(key_));
    }
    // master: the password's key; iv: the stream IV, or the AEAD salt.
    bool Start(const Method& m, bool encrypt, const uint8_t* master, const uint8_t* iv) {
        m_ = &m;
        encrypt_ = encrypt;
        switch (m.kind) {
            case Kind::kCfb:
            case Kind::kCtr:
                std::memcpy(iv_, iv, 16);
                return mbedtls_aes_setkey_enc(&aes_, master, m.key * 8) == 0;
            case Kind::kChacha:
                std::memcpy(key_, master, 32);
                std::memcpy(iv_, iv, 12);
                return true;
            default:   // AEAD
                if (!Subkey(master, m.key, iv, m.iv, key_)) return false;
                std::memset(iv_, 0, sizeof(iv_));
                return m.kind != Kind::kGcm || mbedtls_gcm_setkey(&gcm_, MBEDTLS_CIPHER_ID_AES, key_, m.key * 8) == 0;
        }
    }
    // Stream ciphers: same length out.
    bool Stream(const uint8_t* in, size_t n, uint8_t* out) {
        switch (m_->kind) {
            case Kind::kCfb:
                return mbedtls_aes_crypt_cfb128(&aes_, encrypt_ ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, n, &off_, iv_, in, out) == 0;
            case Kind::kCtr:
                return mbedtls_aes_crypt_ctr(&aes_, n, &off_, iv_, block_, in, out) == 0;
            case Kind::kChacha:
                for (size_t i = 0; i < n; ++i) {
                    if (off_ == 0 || off_ == 64) {   // next 64-byte block of key stream
                        static const uint8_t kZero[64] = {};
                        crypto_chacha20_ietf(block64_, kZero, 64, key_, iv_, counter_++);
                        off_ = 0;
                    }
                    out[i] = in[i] ^ block64_[off_++];
                }
                return true;
            default: return false;
        }
    }
    // AEAD: out gets n + 16 bytes. Every call uses the next nonce.
    bool Seal(const uint8_t* in, size_t n, uint8_t* out) {
        bool ok;
        if (m_->kind == Kind::kGcm) ok = mbedtls_gcm_crypt_and_tag(&gcm_, MBEDTLS_GCM_ENCRYPT, n, iv_, 12, nullptr, 0, in, out, kTag, out + n) == 0;
        else ok = ChachaPoly(in, n, out, out + n, true);
        Next();
        return ok;
    }
    // AEAD: `in` holds n bytes of cipher text followed by the tag.
    bool Open(const uint8_t* in, size_t n, uint8_t* out) {
        bool ok;
        if (m_->kind == Kind::kGcm) ok = mbedtls_gcm_auth_decrypt(&gcm_, n, iv_, 12, nullptr, 0, in + n, kTag, in, out) == 0;
        else ok = ChachaPoly(in, n, out, const_cast<uint8_t*>(in + n), false);
        Next();
        return ok;
    }

private:
    void Next() {   // little-endian counter nonce
        for (size_t i = 0; i < sizeof(iv_) && ++iv_[i] == 0; ++i) {}
    }
    // RFC 8439 without associated data; the X variant first derives a key with HChaCha20.
    bool ChachaPoly(const uint8_t* in, size_t n, uint8_t* out, uint8_t* tag, bool seal) {
        uint8_t key[32], nonce[12] = {};
        if (m_->kind == Kind::kXChachaPoly) {
            crypto_chacha20_h(key, key_, iv_);
            std::memcpy(nonce + 4, iv_ + 16, 8);
        } else {
            std::memcpy(key, key_, 32);
            std::memcpy(nonce, iv_, 12);
        }
        static const uint8_t kZero[64] = {};
        uint8_t poly_key[64];
        crypto_chacha20_ietf(poly_key, kZero, 64, key, nonce, 0);
        const uint8_t* ct = seal ? out : in;
        if (seal) crypto_chacha20_ietf(out, in, n, key, nonce, 1);
        crypto_poly1305_ctx ctx;
        crypto_poly1305_init(&ctx, poly_key);
        crypto_poly1305_update(&ctx, ct, n);
        if (n % 16) crypto_poly1305_update(&ctx, kZero, 16 - n % 16);
        uint8_t lengths[16] = {};
        for (int i = 0; i < 8; ++i) lengths[8 + i] = static_cast<uint8_t>(uint64_t(n) >> (8 * i));
        crypto_poly1305_update(&ctx, lengths, 16);
        uint8_t mac[16];
        crypto_poly1305_final(&ctx, mac);
        bool ok = true;
        if (seal) std::memcpy(tag, mac, 16);
        else {
            uint8_t diff = 0;
            for (int i = 0; i < 16; ++i) diff |= mac[i] ^ tag[i];
            ok = diff == 0;
            if (ok) crypto_chacha20_ietf(out, in, n, key, nonce, 1);
        }
        std::memset(key, 0, sizeof(key));
        std::memset(poly_key, 0, sizeof(poly_key));
        return ok;
    }

    const Method* m_ = nullptr;
    bool encrypt_ = true;
    mbedtls_aes_context aes_;
    mbedtls_gcm_context gcm_;
    uint8_t key_[32] = {};
    uint8_t iv_[24] = {};       // stream IV / AEAD nonce counter
    uint8_t block_[16] = {};    // CTR key stream
    uint8_t block64_[64] = {};  // ChaCha20 key stream
    size_t off_ = 0;
    uint32_t counter_ = 0;
};

// The relay: between the caller's loopback socket and the server. Lives in PSRAM.
struct Relay {
    const Method* m = nullptr;
    int remote = -1, local = -1;
    uint8_t master[32] = {};
    Cipher up, down;
    bool down_started = false;
    // First data from the caller goes out after the salt / IV and the target address, in one packet:
    // shadowsocks-libev rejects an address that arrives on its own.
    uint8_t lead[kMaxSalt] = {};
    size_t lead_size = 0;
    uint8_t address[kMaxAddress] = {};
    size_t address_size = 0;
    uint8_t in[kChunk];
    uint8_t plain[kMaxAddress + kChunk];
    uint8_t out[kMaxSalt + 2 + kTag + kMaxAddress + kChunk + kTag];
    // From the server: bytes not yet decrypted (an AEAD chunk is up to 16 KB + tags).
    uint8_t rx[kMaxSalt + 2 + kTag + kMaxPayload + kTag];
    size_t rx_size = 0;
    size_t want = 0;   // AEAD: payload length of the chunk being read, 0 = reading a length
    ~Relay() {
        std::memset(master, 0, sizeof(master));
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

bool SendAll(int fd, const uint8_t* data, size_t size) {
    while (size > 0) {
        const int n = send(fd, data, size, 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// Caller -> server.
bool Upstream(Relay& r) {
    const int n = recv(r.local, r.in, sizeof(r.in), 0);
    if (n <= 0) return false;
    size_t plain_size = 0, out_size = 0;
    if (r.address_size) {   // first data: address in front of it, salt / IV in front of everything
        std::memcpy(r.plain, r.address, r.address_size);
        plain_size = r.address_size;
        r.address_size = 0;
        std::memcpy(r.out, r.lead, r.lead_size);
        out_size = r.lead_size;
    }
    std::memcpy(r.plain + plain_size, r.in, n);
    plain_size += n;
    if (!Aead(*r.m)) {
        if (!r.up.Stream(r.plain, plain_size, r.out + out_size)) return false;
        return SendAll(r.remote, r.out, out_size + plain_size);
    }
    const uint8_t length[2] = {static_cast<uint8_t>(plain_size >> 8), static_cast<uint8_t>(plain_size)};
    if (!r.up.Seal(length, 2, r.out + out_size) || !r.up.Seal(r.plain, plain_size, r.out + out_size + 2 + kTag)) return false;
    return SendAll(r.remote, r.out, out_size + 2 + kTag + plain_size + kTag);
}

// Server -> caller.
bool Downstream(Relay& r) {
    const int n = recv(r.remote, r.rx + r.rx_size, sizeof(r.rx) - r.rx_size, 0);
    if (n <= 0) return false;
    r.rx_size += n;
    size_t used = 0;
    if (!r.down_started) {   // the server's salt / IV comes first
        if (r.rx_size < r.m->iv) return true;
        if (!r.down.Start(*r.m, false, r.master, r.rx)) return false;
        r.down_started = true;
        used = r.m->iv;
    }
    if (!Aead(*r.m)) {
        const size_t size = r.rx_size - used;   // decrypted in place (CFB, CTR and ChaCha20 allow it)
        bool ok = !size || (r.down.Stream(r.rx + used, size, r.rx + used) && SendAll(r.local, r.rx + used, size));
        r.rx_size = 0;
        return ok;
    }
    while (true) {
        if (!r.want) {
            if (r.rx_size - used < 2 + kTag) break;
            uint8_t length[2];
            if (!r.down.Open(r.rx + used, 2, length)) { ESP_LOGW(TAG, "server data failed to authenticate (wrong password or cipher?)"); return false; }
            used += 2 + kTag;
            r.want = (size_t(length[0]) << 8 | length[1]) & kMaxPayload;
            if (!r.want) return false;
        }
        if (r.rx_size - used < r.want + kTag) break;
        // Decrypt into place (plain text is shorter than what it replaces), then hand it on.
        uint8_t* chunk = r.rx + used;
        if (!r.down.Open(chunk, r.want, chunk)) { ESP_LOGW(TAG, "server data failed to authenticate"); return false; }
        if (!SendAll(r.local, chunk, r.want)) return false;
        used += r.want + kTag;
        r.want = 0;
    }
    std::memmove(r.rx, r.rx + used, r.rx_size - used);
    r.rx_size -= used;
    return true;
}

void RelayTask(void* arg) {
    auto* relay = static_cast<Relay*>(arg);
    const int top = std::max(relay->remote, relay->local) + 1;
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(relay->remote, &fds);
        FD_SET(relay->local, &fds);
        if (select(top, &fds, nullptr, nullptr, nullptr) <= 0) break;
        if (FD_ISSET(relay->local, &fds) && !Upstream(*relay)) break;
        if (FD_ISSET(relay->remote, &fds) && !Downstream(*relay)) break;
    }
    // Either side ending ends both: the caller sees the connection close, the server is let go.
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

int Open(int remote, const net_proxy::Config& proxy, const std::string& host, int port, int* error) {
    const Method* m = Find(proxy.method);
    if (!m || host.empty() || host.size() > 255) {
        close(remote);
        *error = net_proxy::kErrProxyProtocol;
        return -1;
    }
    Relay* relay = NewRelay();
    if (!relay) { close(remote); *error = ENOMEM; return -1; }
    relay->m = m;
    relay->remote = remote;
    esp_fill_random(relay->lead, m->iv);
    relay->lead_size = m->iv;
    if (!MasterKey(proxy.password, relay->master, m->key) || !relay->up.Start(*m, true, relay->master, relay->lead)) {
        FreeRelay(relay);
        *error = net_proxy::kErrProxyAuth;
        return -1;
    }
    // Target in SOCKS5 form: a literal IPv4 address, or a name for the server to resolve.
    uint8_t* a = relay->address;
    size_t size = 0;
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        a[size++] = 1;
        std::memcpy(a + size, &literal.s_addr, 4);
        size += 4;
    } else {
        a[size++] = 3;
        a[size++] = static_cast<uint8_t>(host.size());
        std::memcpy(a + size, host.data(), host.size());
        size += host.size();
    }
    a[size++] = static_cast<uint8_t>((port >> 8) & 0xFF);
    a[size++] = static_cast<uint8_t>(port & 0xFF);
    relay->address_size = size;
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
    if (xTaskCreatePinnedToCoreWithCaps(RelayTask, "ss_relay", kRelayStack, relay, tskIDLE_PRIORITY + 5, nullptr,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        FreeRelay(relay);
        close(outer);
        *error = ENOMEM;
        return -1;
    }
    return outer;
}
}  // namespace

bool Supported(const std::string& method) { return Find(method) != nullptr; }

const char* MethodName(int index) {
    constexpr int kCount = sizeof(kMethods) / sizeof(kMethods[0]);
    return index >= 0 && index < kCount ? kMethods[index].name : nullptr;
}

void Install() { net_proxy::SetShadowsocksOpener(&Open); }

}  // namespace valet::ss
