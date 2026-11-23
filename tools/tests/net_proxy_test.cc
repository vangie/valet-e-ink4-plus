// Host test of components/esp-ml307/src/esp/net_proxy.cc.
// Starts, on 127.0.0.1: a target that greets and echoes, an HTTP proxy and a
// SOCKS5 proxy (each optionally asking for a login), and a few broken proxies.
// Targets are given by a made-up name, so a connection only works when the
// name really went to the proxy unresolved.
#include "net_proxy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <esp_timer.h>

namespace {
int g_failed = 0;
#define CHECK(condition) do { if (!(condition)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_EQ(a, b) do { const long long va = (a), vb = (b); if (va != vb) { ++g_failed; \
    std::printf("FAIL %s:%d  %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, va, vb); } } while (0)

constexpr char kGreeting[] = "hello from the target\n";
constexpr char kTargetName[] = "target.test";

bool ReadN(int fd, void* out, size_t n) {
    char* p = static_cast<char*>(out);
    while (n > 0) {
        const ssize_t got = recv(fd, p, n, 0);
        if (got <= 0) return false;
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}
void WriteAll(int fd, const std::string& data) { (void)!send(fd, data.data(), data.size(), 0); }

// A listening socket on a free port, served by one thread per connection.
class Server {
public:
    explicit Server(std::function<void(int)> handler) : handler_(std::move(handler)) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener_, 8) != 0) std::abort();
        socklen_t length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread([this] {
            for (;;) {
                const int fd = accept(listener_, nullptr, nullptr);
                if (fd < 0) return;
                ++connections_;
                std::lock_guard<std::mutex> lock(mutex_);
                workers_.emplace_back([this, fd] { handler_(fd); close(fd); });
            }
        });
    }
    ~Server() {
        shutdown(listener_, SHUT_RDWR);
        close(listener_);
        accept_thread_.join();
        for (auto& worker : workers_) worker.join();
    }
    int port() const { return port_; }
    int connections() const { return connections_.load(); }

private:
    std::function<void(int)> handler_;
    int listener_ = -1;
    int port_ = 0;
    std::atomic<int> connections_{0};
    std::thread accept_thread_;
    std::mutex mutex_;
    std::vector<std::thread> workers_;
};

int ConnectLocal(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { close(fd); return -1; }
    return fd;
}

// Copy both ways until either side closes.
void Relay(int a, int b) {
    std::thread up([a, b] {
        char buffer[2048];
        for (;;) {
            const ssize_t n = recv(a, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            if (send(b, buffer, n, 0) <= 0) break;
        }
        shutdown(b, SHUT_WR);
    });
    char buffer[2048];
    for (;;) {
        const ssize_t n = recv(b, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        if (send(a, buffer, n, 0) <= 0) break;
    }
    shutdown(a, SHUT_RDWR);
    up.join();
}

struct Seen {
    std::mutex mutex;
    std::string target;   // what the proxy was asked to reach
    std::string login;    // what it was given as a login
    void Set(const std::string& t, const std::string& l) { std::lock_guard<std::mutex> lock(mutex); target = t; login = l; }
    std::string Target() { std::lock_guard<std::mutex> lock(mutex); return target; }
    std::string Login() { std::lock_guard<std::mutex> lock(mutex); return login; }
};

// HTTP proxy: CONNECT only. `login` empty = open to all; otherwise "Basic ..." expected.
std::function<void(int)> HttpProxy(int target_port, std::string expected_auth, Seen* seen, int refuse_status = 0) {
    return [=](int fd) {
        std::string head;
        char c;
        while (head.size() < 4096 && (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0)) {
            if (recv(fd, &c, 1, 0) <= 0) return;
            // Bytes that cannot start a request line get 400 at once, as real HTTP proxies answer.
            if (head.empty() && (c < 'A' || c > 'Z')) { WriteAll(fd, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
            head += c;
        }
        char target[256] = "";
        if (std::sscanf(head.c_str(), "CONNECT %255s HTTP/1.1", target) != 1) { WriteAll(fd, "HTTP/1.1 400 Bad Request\r\n\r\n"); return; }
        std::string auth;
        const size_t at = head.find("Proxy-Authorization: ");
        if (at != std::string::npos) auth = head.substr(at + 21, head.find("\r\n", at) - at - 21);
        seen->Set(target, auth);
        if (!expected_auth.empty() && auth != expected_auth) {
            WriteAll(fd, "HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic\r\n\r\n");
            return;
        }
        if (refuse_status) { WriteAll(fd, "HTTP/1.1 " + std::to_string(refuse_status) + " No\r\nContent-Length: 0\r\n\r\n"); return; }
        if (std::string(target) != std::string(kTargetName) + ":443") { WriteAll(fd, "HTTP/1.1 502 Bad Gateway\r\n\r\n"); return; }
        const int upstream = ConnectLocal(target_port);
        if (upstream < 0) { WriteAll(fd, "HTTP/1.1 502 Bad Gateway\r\n\r\n"); return; }
        // Headers after the status line, as real proxies send.
        WriteAll(fd, "HTTP/1.1 200 Connection established\r\nProxy-Agent: test\r\n\r\n");
        Relay(fd, upstream);
        close(upstream);
    };
}

// SOCKS5 proxy: CONNECT only. `user` empty = no login asked.
std::function<void(int)> SocksProxy(int target_port, std::string user, std::string password, Seen* seen, int reply_code = 0) {
    return [=](int fd) {
        unsigned char head[2];
        if (!ReadN(fd, head, 2) || head[0] != 5) return;   // not SOCKS5: close, as real servers do
        unsigned char methods[255];
        if (!ReadN(fd, methods, head[1])) return;
        const bool offers_login = std::memchr(methods, 2, head[1]) != nullptr;
        const bool offers_none = std::memchr(methods, 0, head[1]) != nullptr;
        std::string login;
        if (!user.empty()) {
            if (!offers_login) { const unsigned char no[2] = {5, 0xFF}; send(fd, no, 2, 0); return; }
            const unsigned char pick[2] = {5, 2};
            send(fd, pick, 2, 0);
            unsigned char v, ulen, plen;
            char u[256] = "", p[256] = "";
            if (!ReadN(fd, &v, 1) || v != 1 || !ReadN(fd, &ulen, 1) || !ReadN(fd, u, ulen) || !ReadN(fd, &plen, 1) || !ReadN(fd, p, plen)) return;
            login = std::string(u, ulen) + ":" + std::string(p, plen);
            const bool ok = login == user + ":" + password;
            const unsigned char verdict[2] = {1, static_cast<unsigned char>(ok ? 0 : 1)};
            send(fd, verdict, 2, 0);
            if (!ok) { seen->Set("", login); return; }
        } else {
            if (!offers_none) return;
            const unsigned char pick[2] = {5, 0};
            send(fd, pick, 2, 0);
        }
        unsigned char request[4];
        if (!ReadN(fd, request, 4) || request[0] != 5 || request[1] != 1) return;
        std::string target;
        if (request[3] == 3) {
            unsigned char length;
            char name[256];
            if (!ReadN(fd, &length, 1) || !ReadN(fd, name, length)) return;
            target = "name:" + std::string(name, length);
        } else if (request[3] == 1) {
            unsigned char ip[4];
            if (!ReadN(fd, ip, 4)) return;
            char text[32];
            std::snprintf(text, sizeof(text), "ip:%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            target = text;
        } else return;
        unsigned char port[2];
        if (!ReadN(fd, port, 2)) return;
        target += ":" + std::to_string((port[0] << 8) | port[1]);
        seen->Set(target, login);
        if (reply_code) { const unsigned char no[10] = {5, static_cast<unsigned char>(reply_code), 0, 1}; send(fd, no, 10, 0); return; }
        const int upstream = ConnectLocal(target_port);
        // Reply with a name-typed bound address: the longest form the client has to skip.
        const std::string reply = std::string("\x05\x00\x00\x03", 4) + std::string(1, static_cast<char>(9)) + "localhost" + std::string("\x1F\x90", 2);
        WriteAll(fd, reply);
        Relay(fd, upstream);
        close(upstream);
    };
}

// Through an opened connection: the target's greeting arrives intact, then an echo.
void CheckTunnel(int fd) {
    CHECK(fd >= 0);
    if (fd < 0) return;
    char greeting[sizeof(kGreeting)] = "";
    CHECK(ReadN(fd, greeting, sizeof(kGreeting) - 1));
    CHECK(std::strcmp(greeting, kGreeting) == 0);
    WriteAll(fd, "ping-1234");
    char echo[10] = "";
    CHECK(ReadN(fd, echo, 9));
    CHECK(std::strcmp(echo, "ping-1234") == 0);
    close(fd);
}

net_proxy::Config Proxy(net_proxy::Kind kind, int port, const char* user = "", const char* password = "") {
    net_proxy::Config config;
    config.kind = kind;
    config.host = "127.0.0.1";
    config.port = port;
    config.user = user;
    config.password = password;
    return config;
}

void TestParse() {
    using net_proxy::Kind;
    using net_proxy::ParseResult;
    net_proxy::Config c;
    c.kind = Kind::kSocks5;
    CHECK(net_proxy::Parse("192.168.0.10:7890", &c) == ParseResult::kOk);
    CHECK(c.kind == Kind::kSocks5 && c.host == "192.168.0.10" && c.port == 7890 && c.user.empty());   // no scheme: kind kept
    CHECK(net_proxy::Parse("  http://proxy.lan:8080/ ", &c) == ParseResult::kOk);
    CHECK(c.kind == Kind::kHttp && c.host == "proxy.lan" && c.port == 8080);
    CHECK(net_proxy::Parse("socks5h://me:p@ss:word@10.0.0.2:1080", &c) == ParseResult::kOk);
    CHECK(c.kind == Kind::kSocks5 && c.host == "10.0.0.2" && c.port == 1080 && c.user == "me" && c.password == "p@ss:word");
    CHECK(net_proxy::Parse("10.0.0.3:1081", &c) == ParseResult::kOk);
    CHECK(c.user == "me" && c.password == "p@ss:word");                 // a login is kept unless the text has one
    CHECK(net_proxy::Parse("SOCKS5://user@h.example:1", &c) == ParseResult::kOk);
    CHECK(c.user == "user" && c.password.empty());
    const net_proxy::Config before = c;
    CHECK(net_proxy::Parse("", &c) == ParseResult::kEmpty);
    CHECK(net_proxy::Parse("   ", &c) == ParseResult::kEmpty);
    CHECK(net_proxy::Parse("proxy.lan", &c) == ParseResult::kBadPort);
    CHECK(net_proxy::Parse("proxy.lan:", &c) == ParseResult::kBadPort);
    CHECK(net_proxy::Parse("proxy.lan:0", &c) == ParseResult::kBadPort);
    CHECK(net_proxy::Parse("proxy.lan:65536", &c) == ParseResult::kBadPort);
    CHECK(net_proxy::Parse("proxy.lan:80a", &c) == ParseResult::kBadPort);
    CHECK(net_proxy::Parse(":8080", &c) == ParseResult::kNoHost);
    CHECK(net_proxy::Parse("a b:8080", &c) == ParseResult::kNoHost);
    CHECK(net_proxy::Parse("https://proxy.lan:443", &c) == ParseResult::kUnsupportedScheme);
    CHECK(net_proxy::Parse("socks4://proxy.lan:1080", &c) == ParseResult::kUnsupportedScheme);
    CHECK(c.host == before.host && c.port == before.port && c.kind == before.kind);   // a rejected text changes nothing

    // Shadowsocks links: SIP002 (Base64 user info) and the all-Base64 form.
    CHECK(net_proxy::Parse("ss://YWVzLTI1Ni1nY206c2VjcmV0@1.2.3.4:8388#name", &c) == ParseResult::kOk);
    CHECK(c.kind == Kind::kShadowsocks && c.host == "1.2.3.4" && c.port == 8388 && c.method == "aes-256-gcm" && c.password == "secret" && c.user.empty());

    // Trojan links: password percent-decoded, sni (or peer) and allowInsecure (or insecure) read, name dropped.
    CHECK(net_proxy::Parse("trojan://p%40ss@relay.example:443?sni=real.example&allowInsecure=1&type=tcp#US%2001", &c) == ParseResult::kOk);
    CHECK(c.kind == Kind::kTrojan && c.host == "relay.example" && c.port == 443 && c.password == "p@ss" && c.user.empty());
    CHECK(c.sni == "real.example" && c.insecure);
    CHECK(net_proxy::Parse("TROJAN://pw@10.1.2.3:4065/?peer=cdn.example&security=tls", &c) == ParseResult::kOk);
    CHECK(c.host == "10.1.2.3" && c.port == 4065 && c.sni == "cdn.example" && !c.insecure);
    CHECK(net_proxy::Parse("trojan://pw@node.example:443?sni=node.example&insecure=true", &c) == ParseResult::kOk);
    CHECK(c.sni.empty() && c.insecure);   // an sni equal to the address is the default
    CHECK(net_proxy::Parse("trojan://pw@node.example:443?type=ws&path=/x", &c) == ParseResult::kUnsupportedTransport);
    CHECK(net_proxy::Parse("trojan://pw@node.example:443?security=reality", &c) == ParseResult::kUnsupportedTransport);
    CHECK(net_proxy::Parse("trojan://node.example:443", &c) == ParseResult::kNoHost);
    CHECK(net_proxy::Parse("trojan://pw@node.example", &c) == ParseResult::kBadPort);
}

void TestRouting() {
    using net_proxy::Kind;
    net_proxy::Set(net_proxy::Config{});
    CHECK(!net_proxy::Applies("github.com"));
    net_proxy::Set(Proxy(Kind::kHttp, 8080));
    CHECK(net_proxy::Applies("github.com"));
    CHECK(net_proxy::Applies("8.8.8.8"));
    CHECK(net_proxy::Applies("172.32.0.1"));        // just outside 172.16/12
    for (const char* local : {"127.0.0.1", "10.1.2.3", "192.168.1.20", "172.16.0.1", "172.31.255.254", "169.254.1.1",
                              "localhost", "LocalHost", "printer.local", "nas", "127.0.0.1" /* the proxy itself */}) {
        CHECK(!net_proxy::Applies(local));
    }
    net_proxy::Config forward;
    CHECK(net_proxy::ForwardsPlainHttp("example.com", &forward) && forward.port == 8080);
    CHECK(!net_proxy::ForwardsPlainHttp("192.168.4.1", nullptr));
    net_proxy::Set(Proxy(Kind::kSocks5, 1080));
    CHECK(!net_proxy::ForwardsPlainHttp("example.com", nullptr));   // SOCKS tunnels plain http like anything else
    CHECK(net_proxy::BasicAuth(Proxy(Kind::kHttp, 1)).empty());
    CHECK(net_proxy::BasicAuth(Proxy(Kind::kHttp, 1, "Aladdin", "open sesame")) == "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    CHECK(net_proxy::BasicAuth(Proxy(Kind::kHttp, 1, "a", "")) == "Basic YTo=");

    // A task-scoped proxy wins over the global one and goes away with its scope; other tasks do not see it.
    {
        net_proxy::ScopedConfig scoped(Proxy(Kind::kHttp, 9999));
        CHECK_EQ(net_proxy::Get().port, 9999);
        int other = 0;
        std::thread([&other] { other = net_proxy::Get().port; }).join();
        CHECK_EQ(other, 1080);
        {
            net_proxy::ScopedConfig off{net_proxy::Config{}};
            CHECK(!net_proxy::Applies("github.com"));
        }
        CHECK_EQ(net_proxy::Get().port, 9999);
    }
    CHECK_EQ(net_proxy::Get().port, 1080);
    net_proxy::Set(net_proxy::Config{});
}

void TestConnections() {
    using net_proxy::Kind;
    Server target([](int fd) {
        WriteAll(fd, kGreeting);   // sent at once: must not be swallowed with the proxy's answer
        char buffer[512];
        for (;;) {
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) return;
            send(fd, buffer, n, 0);
        }
    });
    int error = -1;

    // No proxy: straight to the target.
    net_proxy::Set(net_proxy::Config{});
    CheckTunnel(net_proxy::Open("127.0.0.1", target.port(), 0, &error));
    CHECK_EQ(error, net_proxy::kOk);
    // A name no resolver accepts (a label longer than 63 bytes); a made-up name is not enough,
    // because a machine behind a tunnel may resolve every name to a placeholder address.
    CHECK(net_proxy::Open(std::string(80, 'x') + ".invalid", 80, 0, &error) < 0);
    CHECK_EQ(error, net_proxy::kErrResolve);

    Seen seen;
    {   // HTTP proxy, open to all.
        Server proxy(HttpProxy(target.port(), "", &seen));
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port()));
        CheckTunnel(net_proxy::Open(kTargetName, 443, 0, &error));
        CHECK_EQ(error, net_proxy::kOk);
        CHECK(seen.Target() == "target.test:443");
        CHECK(seen.Login().empty());
        // A local address does not go to the proxy even though one is set.
        const int before = proxy.connections();
        CheckTunnel(net_proxy::Open("127.0.0.1", target.port(), 0, &error));
        CHECK_EQ(proxy.connections(), before);
        // `direct` skips the proxy even for an outside name.
        const int skipped = net_proxy::Open(std::string(80, 'x') + ".invalid", 443, 0, &error, true);
        CHECK(skipped < 0);
        CHECK_EQ(error, net_proxy::kErrResolve);
        CHECK_EQ(proxy.connections(), before);
        // The proxy cannot reach an unknown target.
        CHECK(net_proxy::Open("elsewhere.test", 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyRefused);
        // SOCKS5 spoken to an HTTP proxy.
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyProtocol);
    }
    {   // HTTP proxy that asks for a login.
        Server proxy(HttpProxy(target.port(), "Basic dXNlcjpzM2NyZXQ=", &seen));
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port(), "user", "s3cret"));
        CheckTunnel(net_proxy::Open(kTargetName, 443, 0, &error));
        CHECK(seen.Login() == "Basic dXNlcjpzM2NyZXQ=");
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port(), "user", "wrong"));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyAuth);
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyAuth);
    }
    {   // HTTP proxy that refuses.
        Server proxy(HttpProxy(target.port(), "", &seen, 403));
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyRefused);
    }
    {   // SOCKS5 proxy, open to all: a name goes as a name, an address as an address.
        Server proxy(SocksProxy(target.port(), "", "", &seen));
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port()));
        CheckTunnel(net_proxy::Open(kTargetName, 443, 0, &error));
        CHECK_EQ(error, net_proxy::kOk);
        CHECK(seen.Target() == "name:target.test:443");
        CheckTunnel(net_proxy::Open("93.184.216.34", 8443, 0, &error));
        CHECK(seen.Target() == "ip:93.184.216.34:8443");
        // A login the proxy does not ask for is harmless.
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port(), "user", "s3cret"));
        CheckTunnel(net_proxy::Open(kTargetName, 443, 0, &error));
        // HTTP spoken to a SOCKS5 proxy.
        net_proxy::Set(Proxy(Kind::kHttp, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyProtocol);
    }
    {   // SOCKS5 proxy that asks for a login.
        Server proxy(SocksProxy(target.port(), "user", "s3cret", &seen));
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port(), "user", "s3cret"));
        CheckTunnel(net_proxy::Open(kTargetName, 443, 0, &error));
        CHECK(seen.Login() == "user:s3cret");
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port(), "user", "wrong"));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyAuth);
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyAuth);
    }
    {   // SOCKS5 proxy that cannot reach the target (reply 5: connection refused).
        Server proxy(SocksProxy(target.port(), "", "", &seen, 5));
        net_proxy::Set(Proxy(Kind::kSocks5, proxy.port()));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyRefused);
    }
    {   // Something that accepts and never answers: gives up at the deadline, for both kinds.
        std::atomic<bool> release{false};
        Server silent([&release](int) { while (!release.load()) usleep(20000); });
        for (Kind kind : {Kind::kHttp, Kind::kSocks5}) {
            net_proxy::Set(Proxy(kind, silent.port()));
            const int64_t start = esp_timer_get_time();
            CHECK(net_proxy::Open(kTargetName, 443, start + 700000, &error) < 0);
            const int64_t took = esp_timer_get_time() - start;
            CHECK_EQ(error, net_proxy::kErrProxyNoAnswer);
            CHECK(took > 500000 && took < 2000000);
        }
        // A deadline already passed fails at once instead of waiting for ever.
        const int64_t start = esp_timer_get_time();
        CHECK(net_proxy::Open(kTargetName, 443, start - 1, &error) < 0);
        CHECK(esp_timer_get_time() - start < 500000);
        release = true;
    }
    {   // Nothing listening where the proxy should be.
        int port;
        { Server gone([](int) {}); port = gone.port(); }
        net_proxy::Set(Proxy(Kind::kHttp, port));
        CHECK(net_proxy::Open(kTargetName, 443, 0, &error) < 0);
        CHECK_EQ(error, net_proxy::kErrProxyUnreachable);
    }
    net_proxy::Set(net_proxy::Config{});
}
}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    TestParse();
    TestRouting();
    TestConnections();
    if (g_failed) { std::printf("%d check(s) failed\n", g_failed); return 1; }
    std::printf("net_proxy: all checks passed\n");
    return 0;
}
