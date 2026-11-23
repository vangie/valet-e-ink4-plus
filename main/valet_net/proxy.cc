#include "proxy.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dual_network_board.h"
#include "http.h"
#include "power_policy.h"
#include "shadowsocks.h"
#include "trojan.h"
#include "settings.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>

namespace valet::proxy {
namespace {
constexpr char TAG[] = "Proxy";
constexpr char kStore[] = "proxy";
// Small, public, and a place this firmware needs to reach anyway (the font pack lives there).
// It answers with a redirect and no body.
constexpr char kTestUrl[] = "https://github.com/vangie/valet-e-ink4-plus/releases/download/fonts-1/SHA256SUMS.txt";
constexpr int kErrNoNetwork = 2001;
constexpr int kErrNoAddress = 2002;
constexpr int kErrBadAnswer = 2003;

std::mutex s_mutex;
Prefs s_prefs;

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}

net_proxy::Config ToConfig(const Prefs& prefs) {
    net_proxy::Config config;
    config.kind = prefs.kind;
    config.host = prefs.host;
    config.port = prefs.port;
    config.user = prefs.user;
    config.password = prefs.password;
    config.method = prefs.method;
    config.sni = prefs.sni;
    config.insecure = prefs.insecure;
    return config;
}

bool InForceFor(const Prefs& prefs) {
    return prefs.HasAddress() && prefs.kind != net_proxy::Kind::kNone && (OnWifi() ? prefs.on_wifi : prefs.on_cell) &&
           (!prefs.PasswordOnly() || !prefs.password.empty());
}

void Apply(const Prefs& prefs) {
    const bool on = InForceFor(prefs);
    net_proxy::Set(on ? ToConfig(prefs) : net_proxy::Config{});
    // The address and the login stay out of the log.
    ESP_LOGI(TAG, "%s", !on ? "off" : prefs.kind == net_proxy::Kind::kHttp ? "HTTP proxy in force"
                           : prefs.kind == net_proxy::Kind::kShadowsocks ? "Shadowsocks in force"
                           : prefs.kind == net_proxy::Kind::kTrojan ? "Trojan in force" : "SOCKS5 proxy in force");
}
}  // namespace

bool OnWifi() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual ? dual->GetNetworkType() == NetworkType::WIFI : true;
}

void Init() {
    ss::Install();
    trojan::Install();
    Prefs loaded;
    {
        Settings store(kStore, false);
        loaded.on_wifi = store.GetInt("wifi", 0) != 0;
        loaded.on_cell = store.GetInt("cell", 0) != 0;
        const int kind = store.GetInt("kind", 1);
        loaded.kind = kind == 2 ? net_proxy::Kind::kSocks5 : kind == 3 ? net_proxy::Kind::kShadowsocks
                    : kind == 4 ? net_proxy::Kind::kTrojan : net_proxy::Kind::kHttp;
        loaded.host = store.GetString("host");
        loaded.port = store.GetInt("port", 0);
        loaded.user = store.GetString("user");
        loaded.password = store.GetString("pass");
        loaded.method = store.GetString("method");
        loaded.sni = store.GetString("sni");
        loaded.insecure = store.GetInt("insecure", 0) != 0;
        loaded.more_kind = store.GetInt("more", 3) == 4 ? net_proxy::Kind::kTrojan : net_proxy::Kind::kShadowsocks;
    }
    if (loaded.port < 1 || loaded.port > 65535) loaded.port = 0;
    if (loaded.PasswordOnly()) loaded.more_kind = loaded.kind;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_prefs = loaded;
    }
    Apply(loaded);
}

Prefs Get() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_prefs;
}

bool Update(const Prefs& prefs) {
    // NVS is written from the storage worker: this may be called from the UI task.
    if (!Application::GetInstance().Schedule([prefs]() {
        Settings store(kStore, true);
        store.SetInt("wifi", prefs.on_wifi ? 1 : 0);
        store.SetInt("cell", prefs.on_cell ? 1 : 0);
        store.SetInt("kind", static_cast<int>(prefs.kind == net_proxy::Kind::kNone ? net_proxy::Kind::kHttp : prefs.kind));
        store.SetString("host", prefs.host);
        store.SetInt("port", prefs.port);
        store.SetString("user", prefs.user);
        store.SetString("pass", prefs.password);
        store.SetString("method", prefs.method);
        store.SetString("sni", prefs.sni);
        store.SetInt("insecure", prefs.insecure ? 1 : 0);
        store.SetInt("more", static_cast<int>(prefs.more_kind));
        if (!store.Commit()) ESP_LOGE(TAG, "the proxy choice was not saved");
    }, Application::TaskDomain::Storage)) return false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_prefs = prefs;
    }
    Apply(prefs);
    return true;
}

bool InForce() { return InForceFor(Get()); }

TestResult Test(const Prefs& prefs) {
    TestResult result;
    result.kind = prefs.kind;
    const bool ss = prefs.kind == net_proxy::Kind::kShadowsocks;
    if (!prefs.HasAddress()) { result.error = kErrNoAddress; return result; }
    if (prefs.PasswordOnly() && prefs.password.empty()) { result.error = net_proxy::kErrProxyAuth; return result; }
    if (ss && !ss::Supported(prefs.method)) { result.error = net_proxy::kErrProxyProtocol; return result; }
    // Keep the radio up for the request; it is parked soon after the last use.
    PowerNeedHold hold(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(20000)) { result.error = kErrNoNetwork; return result; }
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) { result.error = kErrNoNetwork; return result; }
    // For this task only: the proxy being tested, whether or not its switch is on.
    net_proxy::ScopedConfig scoped(ToConfig(prefs));
    const int64_t start = esp_timer_get_time();
    http->SetTimeout(15000);
    http->SetRequestDeadline(start + 20000000);
    http->SetHeader("User-Agent", "valet-e-ink");
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    if (!http->Open("GET", kTestUrl)) {
        result.error = http->GetLastError();
        if (result.error == 0) result.error = -1;
        http->Close();
        return result;
    }
    result.status = http->GetStatusCode();
    http->Close();
    result.ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
    result.ok = result.status >= 200 && result.status < 400;
    if (!result.ok) result.error = kErrBadAnswer;
    ESP_LOGI(TAG, "test: %s status=%d error=%d %d ms", result.ok ? "ok" : "failed", result.status, result.error, result.ms);
    return result;
}

std::string Describe(const TestResult& result) {
    char text[200];
    if (result.ok) {
        std::snprintf(text, sizeof(text), Text("通了：经代理连到 GitHub 用了 %d.%d 秒。", "Works: reached GitHub through the proxy in %d.%d s."),
                      result.ms / 1000, result.ms % 1000 / 100);
        return text;
    }
    const bool ss = result.kind == net_proxy::Kind::kShadowsocks;
    const bool trojan = result.kind == net_proxy::Kind::kTrojan;
    switch (result.error) {
        case kErrNoAddress: return Text("还没填代理的地址。", "No proxy address yet.");
        case kErrNoNetwork: return Text("设备自己还没连上网。", "The device itself is not online.");
        case net_proxy::kErrProxyUnreachable:
            return Text("连不上代理。看看地址和端口对不对，代理有没有允许别的设备来连。",
                        "Cannot reach the proxy. Check the address and port, and that it accepts other devices.");
        case net_proxy::kErrProxyAuth:
            if (ss) return Text("SS 要填密码。", "SS needs a password.");
            if (trojan) return Text("Trojan 要填密码。", "Trojan needs a password.");
            return Text("代理要账号和密码，或者填的不对。", "The proxy wants a user name and password, or these are wrong.");
        case net_proxy::kErrProxyRefused:
            return Text("代理连上了，但它没能连到 GitHub。", "The proxy answered but could not reach GitHub.");
        case net_proxy::kErrProxyNoAnswer:
            if (trojan) return Text("连上了服务器，但 TLS 握手一直没完成。端口是不是 Trojan 的？",
                                    "Reached the server, but the TLS handshake never finished. Is this the Trojan port?");
            return Text("代理连上了，但一直不回话。类型（HTTP / SOCKS5）可能选反了。",
                        "Connected to the proxy, but it never answered. The type (HTTP / SOCKS5) may be the other one.");
        case net_proxy::kErrProxyProtocol:
            if (ss) return Text("设备不支持这种加密方式。", "The device does not support this cipher.");
            return Text("那个端口回的不是这种代理的话。类型（HTTP / SOCKS5）是不是选反了？",
                        "That port does not speak this kind of proxy. Is the type (HTTP / SOCKS5) the other one?");
        case net_proxy::kErrProxyTls:
            return Text("连上了服务器，但 TLS 没建成。看看端口和服务器名（SNI）对不对。",
                        "Reached the server, but TLS failed. Check the port and the server name (SNI).");
        case net_proxy::kErrProxyCertificate:
            return Text("服务器的证书和服务器名（SNI）对不上。节点要求不检查证书的话，把「检查证书」关掉。",
                        "The server's certificate does not match the server name (SNI). If the node says to skip the check, turn off the certificate check.");
        case kErrBadAnswer:
            std::snprintf(text, sizeof(text), Text("经代理连到了，但 GitHub 回的是 %d。", "Reached through the proxy, but GitHub answered %d."), result.status);
            return text;
        default:
            // Shadowsocks has no answer of its own: a wrong password or cipher only shows as a connection that breaks.
            if (trojan) {
                std::snprintf(text, sizeof(text), Text("和服务器的 TLS 建好了，但经它的连接没通（代码 %d）。多半是密码不对。",
                                                       "TLS with the server works, but the connection through it failed (code %d). Most likely the password is wrong."), result.error);
                return text;
            }
            if (ss) {
                std::snprintf(text, sizeof(text), Text("连上了服务器，但加密连接没建成（代码 %d）。多半是密码或加密方式和服务器不一致。",
                                                       "Reached the server, but the secure connection failed (code %d). Most likely the password or the cipher differs from the server's."), result.error);
                return text;
            }
            std::snprintf(text, sizeof(text), Text("代理接通了，后面的加密连接没建成（代码 %d）。", "The proxy opened the connection, but the secure connection failed (code %d)."), result.error);
            return text;
    }
}

}  // namespace valet::proxy
