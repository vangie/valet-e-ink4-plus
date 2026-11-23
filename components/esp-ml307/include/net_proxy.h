#pragma once

// An outgoing proxy for the TCP and TLS connections of this component
// (EspTcp, EspSsl, and through them HttpClient and WebSocket).
//
// HTTP proxy: https and ws/wss targets use a CONNECT tunnel; a plain http
// request is sent to the proxy with an absolute URI (HttpClient does that).
// SOCKS5: every target uses a CONNECT command, optionally with a user name
// and password (RFC 1928 / 1929).
// Shadowsocks: every byte to the server is encrypted. This component only
// connects to the server; the cipher and the relay that encrypts are installed
// by the application (SetShadowsocksOpener), which hands back an ordinary socket.
// In all cases the target's name goes to the proxy unresolved, so the
// device does no DNS lookup of its own for a proxied connection.
//
// Not covered: UDP (time sync, call media), and code that opens its own
// sockets instead of going through this component.

#include <cstdint>
#include <string>

namespace net_proxy {

enum class Kind : uint8_t { kNone = 0, kHttp = 1, kSocks5 = 2, kShadowsocks = 3 };



struct Config {
    Kind kind = Kind::kNone;
    std::string host;
    int port = 0;
    std::string user;       // not used by Shadowsocks
    std::string password;   // Shadowsocks: the server's password (required)
    std::string method;     // Shadowsocks: cipher name, lower case (e.g. "aes-256-gcm")
};

/** @brief Turns a TCP connection to a Shadowsocks server into a socket that reaches host:port.
 *         Takes ownership of `server_fd`. Returns the socket, or -1 with `error` set. */
using ShadowsocksOpener = int (*)(int server_fd, const Config& proxy, const std::string& host, int port, int* error);
/** @brief Install the Shadowsocks implementation; until then a Shadowsocks proxy fails to connect. */
void SetShadowsocksOpener(ShadowsocksOpener opener);

// What Open() reports through `error`. Other values are errno.
enum Error : int {
    kOk = 0,
    kErrResolve = 1001,          // direct connection: the name did not resolve
    kErrProxyUnreachable = 1002, // no TCP connection to the proxy
    kErrProxyAuth = 1003,        // the proxy wants a user name and password, or refused them
    kErrProxyRefused = 1004,     // the proxy would not, or could not, connect to the target
    kErrProxyNoAnswer = 1005,    // the proxy accepted the connection and then said nothing in time
    kErrProxyProtocol = 1006,    // the answer was not that of a proxy of this kind
};

/** @brief Set the proxy used by connections opened from now on; Kind::kNone turns it off. */
void Set(const Config& config);
/** @brief The proxy in force for the calling task (see ScopedConfig), else the one from Set(). */
Config Get();

/** @brief Use `config` for connections opened by the calling task while this object lives. */
class ScopedConfig {
public:
    explicit ScopedConfig(const Config& config);
    ~ScopedConfig();
    ScopedConfig(const ScopedConfig&) = delete;
    ScopedConfig& operator=(const ScopedConfig&) = delete;

private:
    Config config_;
    const Config* previous_;
};

/** @brief True when a connection to `host` would go through the proxy: one is set and
 *         `host` is not on the local network (private address, `localhost`, `.local`, a bare name). */
bool Applies(const std::string& host);

/** @brief Open a connected, blocking TCP socket that reaches host:port, through the proxy
 *         when one applies (`direct` skips it). Returns the socket, or -1 with `error` set.
 *         `deadline_us` is an esp_timer time, 0 for the default of 15 s per step. */
int Open(const std::string& host, int port, int64_t deadline_us, int* error, bool direct = false);

/** @brief True when a plain http request to `host` has to be sent to an HTTP proxy with an
 *         absolute URI; `proxy` then holds where to send it. */
bool ForwardsPlainHttp(const std::string& host, Config* proxy);

/** @brief Value for a Proxy-Authorization header, or empty when the proxy has no user name. */
std::string BasicAuth(const Config& proxy);

enum class ParseResult : uint8_t { kOk, kEmpty, kNoHost, kBadPort, kUnsupportedScheme };
/** @brief Read "[scheme://][user[:password]@]host:port". A scheme sets the kind (http, socks5,
 *         socks5h, socks); without one `out->kind` is left as it was. User and password are
 *         replaced only when the text has them.
 *         Also reads Shadowsocks links: "ss://BASE64(method:password)@host:port[#name]" (SIP002, the
 *         user info may also be percent-encoded) and "ss://BASE64(method:password@host:port)[#name]";
 *         the method is stored as given (lower case); whether it is supported is for the caller. */
ParseResult Parse(const std::string& text, Config* out);

}  // namespace net_proxy
