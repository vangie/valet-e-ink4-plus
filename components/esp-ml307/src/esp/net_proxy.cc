#include "net_proxy.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_rom_md5.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// mbedtls 4 keeps the block-cipher API private; the AES-CFB function is still built in.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/aes.h"

namespace net_proxy {
namespace {
constexpr char TAG[] = "NetProxy";
constexpr int64_t kStepUs = 15 * 1000000LL;  // one connect, or one exchange with the proxy
constexpr size_t kMaxAnswer = 4096;          // an HTTP proxy's answer to CONNECT

std::mutex s_mutex;
Config s_config;
thread_local const Config* t_override = nullptr;

int64_t Remaining(int64_t deadline_us) {
    if (!deadline_us) return kStepUs;
    return std::clamp<int64_t>(deadline_us - esp_timer_get_time(), 0, kStepUs);
}

// A zero timeout means "wait for ever" to the socket layer, so zero is never passed for "no time left".
void SetIoTimeout(int fd, int64_t us) {
    timeval tv{};
    tv.tv_sec = us / 1000000;
    tv.tv_usec = us % 1000000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool SendAll(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        const int n = send(fd, bytes, size, 0);
        if (n <= 0) return false;
        bytes += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// 1 = read, 0 = the peer closed or reset the connection, -1 = nothing came in time.
int RecvAll(int fd, void* data, size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size > 0) {
        const int n = recv(fd, bytes, size, 0);
        if (n == 0) return 0;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT ? -1 : 0;
        bytes += n;
        size -= static_cast<size_t>(n);
    }
    return 1;
}

int ConnectDirect(const std::string& host, int port, int64_t deadline_us, int* error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    char service[8];
    std::snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host.c_str(), service, &hints, &list) != 0 || list == nullptr) {
        *error = kErrResolve;
        return -1;
    }
    const int fd = socket(list->ai_family, list->ai_socktype, 0);
    if (fd < 0) {
        *error = errno;
        freeaddrinfo(list);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int ret = connect(fd, list->ai_addr, list->ai_addrlen);
    const int connect_error = ret < 0 ? errno : 0;
    freeaddrinfo(list);
    if (ret < 0 && (connect_error == EINPROGRESS || connect_error == EWOULDBLOCK)) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        const int64_t wait_us = Remaining(deadline_us);
        timeval tv{};
        tv.tv_sec = wait_us / 1000000;
        tv.tv_usec = wait_us % 1000000;
        ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
        const int select_error = ret < 0 ? errno : 0;
        if (ret <= 0) {
            *error = ret == 0 ? ETIMEDOUT : select_error;
            close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        const int result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (result < 0 || so_error != 0) {
            *error = result < 0 ? errno : so_error;
            close(fd);
            return -1;
        }
    } else if (ret < 0) {
        *error = connect_error;
        close(fd);
        return -1;
    }
    if (flags >= 0) fcntl(fd, F_SETFL, flags);
    return fd;
}

std::string Base64(const std::string& in) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(in[i]);
        const unsigned b1 = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned b2 = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        out += kTable[b0 >> 2];
        out += kTable[((b0 & 3) << 4) | (b1 >> 4)];
        out += i + 1 < in.size() ? kTable[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out += i + 2 < in.size() ? kTable[b2 & 63] : '=';
    }
    return out;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

bool EndsWith(const std::string& text, const char* tail) {
    const size_t n = std::strlen(tail);
    return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

// Addresses the proxy cannot be expected to reach: they are on the device's own network.
bool IsLocal(const std::string& host) {
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
        const uint32_t ip = ntohl(address.s_addr);
        return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 ||
               (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
    }
    const std::string name = Lower(host);
    return name == "localhost" || EndsWith(name, ".local") || name.find('.') == std::string::npos;
}

bool AppliesWith(const Config& proxy, const std::string& host) {
    return proxy.kind != Kind::kNone && !proxy.host.empty() && proxy.port > 0 &&
           !IsLocal(host) && Lower(host) != Lower(proxy.host);
}

// CONNECT through an HTTP proxy. The answer is read one byte at a time: whatever
// follows the blank line already belongs to the tunnel.
int HttpConnect(int fd, const Config& proxy, const std::string& host, int port) {
    const std::string target = host + ":" + std::to_string(port);
    std::string request = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n";
    const std::string auth = BasicAuth(proxy);
    if (!auth.empty()) request += "Proxy-Authorization: " + auth + "\r\n";
    request += "\r\n";
    if (!SendAll(fd, request.data(), request.size())) return kErrProxyNoAnswer;
    std::string answer;
    while (answer.size() < kMaxAnswer && !EndsWith(answer, "\r\n\r\n")) {
        char c = 0;
        const int n = RecvAll(fd, &c, 1);
        if (n <= 0) return answer.empty() && n < 0 ? kErrProxyNoAnswer : kErrProxyProtocol;
        answer += c;
    }
    int status = 0;
    if (answer.compare(0, 5, "HTTP/") != 0 || std::sscanf(answer.c_str(), "HTTP/%*s %d", &status) != 1) {
        return kErrProxyProtocol;
    }
    if (status >= 200 && status < 300) return EndsWith(answer, "\r\n\r\n") ? kOk : kErrProxyProtocol;
    return status == 407 ? kErrProxyAuth : kErrProxyRefused;
}

int Socks5Connect(int fd, const Config& proxy, const std::string& host, int port) {
    if (host.size() > 255 || proxy.user.size() > 255 || proxy.password.size() > 255) return kErrProxyProtocol;
    const bool has_login = !proxy.user.empty();
    const unsigned char hello[4] = {5, static_cast<unsigned char>(has_login ? 2 : 1), 0, 2};
    if (!SendAll(fd, hello, has_login ? 4 : 3)) return kErrProxyNoAnswer;
    unsigned char choice[2];
    int n = RecvAll(fd, choice, 2);
    if (n <= 0) return n < 0 ? kErrProxyNoAnswer : kErrProxyProtocol;
    if (choice[0] != 5) return kErrProxyProtocol;
    if (choice[1] == 2 && has_login) {
        std::string login;
        login += static_cast<char>(1);
        login += static_cast<char>(proxy.user.size());
        login += proxy.user;
        login += static_cast<char>(proxy.password.size());
        login += proxy.password;
        if (!SendAll(fd, login.data(), login.size())) return kErrProxyNoAnswer;
        unsigned char verdict[2];
        n = RecvAll(fd, verdict, 2);
        // A proxy that refuses a login may simply close the connection.
        if (n <= 0) return n < 0 ? kErrProxyNoAnswer : kErrProxyAuth;
        if (verdict[1] != 0) return kErrProxyAuth;
    } else if (choice[1] != 0) {
        // 0xFF: none of the offered methods; 2 without a login to give: it wants one.
        return kErrProxyAuth;
    }
    std::string request;
    request += static_cast<char>(5);
    request += static_cast<char>(1);  // CONNECT
    request += static_cast<char>(0);
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        request += static_cast<char>(1);
        request.append(reinterpret_cast<const char*>(&literal.s_addr), 4);
    } else {
        request += static_cast<char>(3);  // a name: the proxy resolves it
        request += static_cast<char>(host.size());
        request += host;
    }
    request += static_cast<char>((port >> 8) & 0xFF);
    request += static_cast<char>(port & 0xFF);
    if (!SendAll(fd, request.data(), request.size())) return kErrProxyNoAnswer;
    unsigned char reply[4];
    n = RecvAll(fd, reply, 4);
    if (n <= 0) return n < 0 ? kErrProxyNoAnswer : kErrProxyRefused;
    if (reply[0] != 5) return kErrProxyProtocol;
    if (reply[1] != 0) return kErrProxyRefused;
    // The bound address is of no use here, but it has to be taken off the stream.
    size_t rest = 0;
    if (reply[3] == 1) rest = 4 + 2;
    else if (reply[3] == 4) rest = 16 + 2;
    else if (reply[3] == 3) {
        unsigned char length = 0;
        if (RecvAll(fd, &length, 1) <= 0) return kErrProxyProtocol;
        rest = static_cast<size_t>(length) + 2;
    } else return kErrProxyProtocol;
    unsigned char skip[32];
    while (rest > 0) {
        const size_t part = std::min(rest, sizeof(skip));
        if (RecvAll(fd, skip, part) <= 0) return kErrProxyProtocol;
        rest -= part;
    }
    return kOk;
}

// ── Shadowsocks, aes-256-cfb ──────────────────────────────────────────────────
// The client sends a random 16-byte IV, then everything encrypted in one CFB stream that starts
// with the target address (SOCKS5 form). The server answers with its own IV and its stream.
// Ciphertext and plaintext have the same length, so nothing needs buffering. The IV and address go out
// in the same packet as the first data: shadowsocks-libev rejects a connection whose address arrived
// alone (seen with 3.3.6: "authentication error" on the next packet), and ss-local never sends it alone.
constexpr size_t kSsChunk = 1460;
constexpr uint32_t kSsRelayStack = 4096;

bool SsKey(const std::string& password, uint8_t key[32]) {
    if (password.empty() || password.size() > 128) return false;
    uint8_t block[16];
    for (size_t filled = 0; filled < 32; filled += 16) {   // EVP_BytesToKey with MD5, as Shadowsocks does
        md5_context_t md5;
        esp_rom_md5_init(&md5);
        if (filled) esp_rom_md5_update(&md5, block, sizeof(block));
        esp_rom_md5_update(&md5, password.data(), password.size());
        esp_rom_md5_final(block, &md5);
        std::memcpy(key + filled, block, 16);
    }
    std::memset(block, 0, sizeof(block));
    return true;
}

struct SsRelay {
    int remote = -1, local = -1;
    mbedtls_aes_context aes;
    uint8_t enc_iv[16] = {}, dec_iv[16] = {};
    size_t enc_off = 0, dec_off = 0, dec_have = 0;   // dec_have: bytes of the server's IV read so far
    uint8_t hello[16 + 4 + 255] = {};                  // IV + encrypted address, sent with the first data
    size_t hello_size = 0;
    uint8_t in[kSsChunk], out[sizeof(hello) + kSsChunk];
    SsRelay() { mbedtls_aes_init(&aes); }
    ~SsRelay() {
        mbedtls_aes_free(&aes);
        if (remote >= 0) close(remote);
        if (local >= 0) close(local);
    }
    bool Encrypt(const uint8_t* data, size_t n, uint8_t* to) {
        return mbedtls_aes_crypt_cfb128(&aes, MBEDTLS_AES_ENCRYPT, n, &enc_off, enc_iv, data, to) == 0;
    }
};

// Copies one chunk from `from` to `to`, encrypting or decrypting. false = this side is done.
bool SsPump(SsRelay& r, bool upstream) {
    const int from = upstream ? r.local : r.remote, to = upstream ? r.remote : r.local;
    const int n = recv(from, r.in, sizeof(r.in), 0);
    if (n <= 0) return false;
    const uint8_t* data = r.in;
    size_t size = static_cast<size_t>(n);
    if (!upstream && r.dec_have < sizeof(r.dec_iv)) {
        const size_t take = std::min(size, sizeof(r.dec_iv) - r.dec_have);
        std::memcpy(r.dec_iv + r.dec_have, data, take);
        r.dec_have += take; data += take; size -= take;
        if (size == 0) return true;
    }
    size_t lead = 0;
    if (upstream && r.hello_size) {   // first data: IV and address in front of it
        std::memcpy(r.out, r.hello, r.hello_size);
        lead = r.hello_size;
        r.hello_size = 0;
    }
    const bool ok = upstream ? r.Encrypt(data, size, r.out + lead)
                             : mbedtls_aes_crypt_cfb128(&r.aes, MBEDTLS_AES_DECRYPT, size, &r.dec_off, r.dec_iv, data, r.out) == 0;
    return ok && SendAll(to, r.out, lead + size);
}

void SsRelayTask(void* arg) {
    auto* relay = static_cast<SsRelay*>(arg);
    const int top = std::max(relay->remote, relay->local) + 1;
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(relay->remote, &fds);
        FD_SET(relay->local, &fds);
        if (select(top, &fds, nullptr, nullptr, nullptr) <= 0) break;
        if (FD_ISSET(relay->local, &fds) && !SsPump(*relay, true)) break;
        if (FD_ISSET(relay->remote, &fds) && !SsPump(*relay, false)) break;
    }
    // Either side ending ends both: the caller sees the connection close, the server is let go.
    shutdown(relay->local, SHUT_RDWR);
    shutdown(relay->remote, SHUT_RDWR);
    delete relay;
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

int ShadowsocksOpen(int remote, const Config& proxy, const std::string& host, int port, int* error) {
    if (host.empty() || host.size() > 255) { close(remote); *error = kErrProxyProtocol; return -1; }
    auto* relay = new (std::nothrow) SsRelay();
    if (!relay) { close(remote); *error = ENOMEM; return -1; }
    relay->remote = remote;
    uint8_t key[32];
    const bool keyed = SsKey(proxy.password, key) && mbedtls_aes_setkey_enc(&relay->aes, key, 256) == 0;
    std::memset(key, 0, sizeof(key));
    if (!keyed) { delete relay; *error = kErrProxyAuth; return -1; }
    esp_fill_random(relay->enc_iv, sizeof(relay->enc_iv));
    // IV, then the encrypted target: a literal IPv4 address, or a name for the server to resolve.
    uint8_t* hello = relay->hello;
    std::memcpy(hello, relay->enc_iv, 16);
    uint8_t target[4 + 255];
    size_t target_size = 0;
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        target[0] = 1;
        std::memcpy(target + 1, &literal.s_addr, 4);
        target_size = 5;
    } else {
        target[0] = 3;
        target[1] = static_cast<uint8_t>(host.size());
        std::memcpy(target + 2, host.data(), host.size());
        target_size = 2 + host.size();
    }
    target[target_size++] = static_cast<uint8_t>((port >> 8) & 0xFF);
    target[target_size++] = static_cast<uint8_t>(port & 0xFF);
    if (!relay->Encrypt(target, target_size, hello + 16)) {
        delete relay;
        *error = kErrProxyProtocol;
        return -1;
    }
    relay->hello_size = 16 + target_size;
    SetIoTimeout(remote, 0);
    int outer = -1;
    if (!LoopbackPair(&outer, &relay->local)) {
        delete relay;
        *error = errno ? errno : EMFILE;
        return -1;
    }
    // The relay only moves bytes (no flash or NVS), so its stack may live in PSRAM.
    if (xTaskCreatePinnedToCoreWithCaps(SsRelayTask, "ss_relay", kSsRelayStack, relay, tskIDLE_PRIORITY + 5, nullptr,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        delete relay;
        close(outer);
        *error = ENOMEM;
        return -1;
    }
    return outer;
}

// Base64 (standard or URL alphabet, padding optional). Empty on bad input.
std::string Base64Decode(const std::string& in) {
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else return "";
        bits = (bits << 6) | static_cast<uint32_t>(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xFF);
        }
    }
    return out;
}

std::string PercentDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else out += in[i];
    }
    return out;
}
}  // namespace

void Set(const Config& config) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_config = config;
}

Config Get() {
    if (t_override) return *t_override;
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_config;
}

ScopedConfig::ScopedConfig(const Config& config) : config_(config), previous_(t_override) {
    t_override = &config_;
}

ScopedConfig::~ScopedConfig() { t_override = previous_; }

bool Applies(const std::string& host) { return AppliesWith(Get(), host); }

int Open(const std::string& host, int port, int64_t deadline_us, int* error, bool direct) {
    int ignored = 0;
    if (!error) error = &ignored;
    *error = kOk;
    const Config proxy = Get();
    if (direct || !AppliesWith(proxy, host)) return ConnectDirect(host, port, deadline_us, error);

    const int fd = ConnectDirect(proxy.host, proxy.port, deadline_us, error);
    if (fd < 0) {
        ESP_LOGW(TAG, "cannot reach the proxy (code %d)", *error);
        *error = kErrProxyUnreachable;
        return -1;
    }
    const int64_t wait_us = Remaining(deadline_us);
    if (wait_us <= 0) {
        close(fd);
        *error = kErrProxyNoAnswer;
        return -1;
    }
    SetIoTimeout(fd, wait_us);
    if (proxy.kind == Kind::kShadowsocks) {
        const int relayed = ShadowsocksOpen(fd, proxy, host, port, error);
        if (relayed < 0) ESP_LOGW(TAG, "Shadowsocks connection not opened (code %d)", *error);
        return relayed;
    }
    const int result = proxy.kind == Kind::kHttp ? HttpConnect(fd, proxy, host, port)
                                                 : Socks5Connect(fd, proxy, host, port);
    if (result != kOk) {
        // Neither the target nor the login is logged.
        ESP_LOGW(TAG, "%s proxy did not open the connection (code %d)",
                 proxy.kind == Kind::kHttp ? "HTTP" : "SOCKS5", result);
        close(fd);
        *error = result;
        return -1;
    }
    SetIoTimeout(fd, 0);  // back to blocking without a limit, like a socket connected directly
    return fd;
}

bool ForwardsPlainHttp(const std::string& host, Config* proxy) {
    Config current = Get();
    if (current.kind != Kind::kHttp || !AppliesWith(current, host)) return false;
    if (proxy) *proxy = std::move(current);
    return true;
}

std::string BasicAuth(const Config& proxy) {
    if (proxy.user.empty()) return "";
    return "Basic " + Base64(proxy.user + ":" + proxy.password);
}

ParseResult Parse(const std::string& text, Config* out) {
    size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string rest = text.substr(begin, end - begin);
    if (rest.empty()) return ParseResult::kEmpty;

    if (Lower(rest.substr(0, 5)) == "ss://") {
        std::string body = rest.substr(5);
        const size_t tag = body.find('#');
        if (tag != std::string::npos) body.resize(tag);
        const size_t query = body.find_first_of("/?");
        if (query != std::string::npos) body.resize(query);   // SIP003 plugins are not supported; the address is kept
        std::string info;
        const size_t at = body.rfind('@');
        if (at == std::string::npos) {   // the whole of "method:password@host:port" is Base64
            body = Base64Decode(body);
            const size_t inner = body.rfind('@');
            if (inner == std::string::npos) return ParseResult::kNoHost;
            info = body.substr(0, inner);
            body = body.substr(inner + 1);
        } else {
            info = body.substr(0, at);
            body = body.substr(at + 1);
            info = info.find(':') != std::string::npos ? PercentDecode(info) : Base64Decode(info);
        }
        const size_t colon = info.find(':');
        if (colon == std::string::npos) return ParseResult::kNoHost;
        if (Lower(info.substr(0, colon)) != kShadowsocksMethod) return ParseResult::kUnsupportedCipher;
        Config parsed = *out;
        parsed.kind = Kind::kShadowsocks;
        const ParseResult address = Parse(body, &parsed);
        if (address != ParseResult::kOk) return address;
        parsed.kind = Kind::kShadowsocks;
        parsed.user.clear();
        parsed.password = info.substr(colon + 1);
        *out = parsed;
        return ParseResult::kOk;
    }

    Kind kind = out->kind;
    const size_t scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) {
        const std::string scheme = Lower(rest.substr(0, scheme_end));
        if (scheme == "http") kind = Kind::kHttp;
        else if (scheme == "socks5" || scheme == "socks5h" || scheme == "socks") kind = Kind::kSocks5;
        else return ParseResult::kUnsupportedScheme;  // https:// (a proxy reached over TLS), socks4://
        rest = rest.substr(scheme_end + 3);
    }
    while (!rest.empty() && rest.back() == '/') rest.pop_back();

    bool has_login = false;
    std::string user, password;
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) {
        const std::string login = rest.substr(0, at);
        const size_t colon = login.find(':');
        user = login.substr(0, colon);
        if (colon != std::string::npos) password = login.substr(colon + 1);
        has_login = true;
        rest = rest.substr(at + 1);
    }

    const size_t colon = rest.rfind(':');
    const std::string host = rest.substr(0, colon);
    if (host.empty() || host.find_first_of(" /:@") != std::string::npos) return ParseResult::kNoHost;
    if (colon == std::string::npos || colon + 1 >= rest.size()) return ParseResult::kBadPort;
    const std::string port_text = rest.substr(colon + 1);
    char* tail = nullptr;
    const long port = std::strtol(port_text.c_str(), &tail, 10);
    if (*tail != '\0' || port < 1 || port > 65535) return ParseResult::kBadPort;

    out->kind = kind;
    out->host = host;
    out->port = static_cast<int>(port);
    if (has_login) {
        out->user = user;
        out->password = password;
    }
    return ParseResult::kOk;
}

}  // namespace net_proxy
