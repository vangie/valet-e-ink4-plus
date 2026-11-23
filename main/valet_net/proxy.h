#pragma once

// The user's proxy choice: one proxy address, and a switch for each way of
// getting online (Wi-Fi, 4G). Stored in NVS namespace "proxy"; applied to the
// network layer through net_proxy::Set().

#include <string>

#include "net_proxy.h"

namespace valet::proxy {

struct Prefs {
    bool on_wifi = false;   // use the proxy while Wi-Fi is the connection
    bool on_cell = false;   // use the proxy while 4G is the connection
    net_proxy::Kind kind = net_proxy::Kind::kHttp;
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
    std::string method;     // Shadowsocks cipher; empty = valet::ss::kDefaultMethod
    std::string sni;        // Trojan: server name for TLS; empty = the address
    bool insecure = false;  // Trojan: do not check the server's certificate
    /** @brief The protocol last used under 「更多协议」 (kShadowsocks or kTrojan): where that choice goes back to. */
    net_proxy::Kind more_kind = net_proxy::Kind::kShadowsocks;
    /** @brief Shadowsocks and Trojan need a password and have no user name. */
    bool PasswordOnly() const { return kind == net_proxy::Kind::kShadowsocks || kind == net_proxy::Kind::kTrojan; }
    bool HasAddress() const { return !host.empty() && port > 0; }
};

/** @brief Load the stored choice and apply it. Call once at start-up, before the network is used. */
void Init();
/** @brief The current choice (a copy). */
Prefs Get();
/** @brief Replace the choice: in force for connections opened from now on, and saved in the
 *         background. Returns false when the save could not be queued (nothing is changed). */
bool Update(const Prefs& prefs);
/** @brief True when Wi-Fi is the connection in use (the other one is 4G). */
bool OnWifi();
/** @brief True when new connections go through the proxy right now. */
bool InForce();

struct TestResult {
    bool ok = false;
    int error = 0;    // net_proxy::Error, or another code from the connection
    int status = 0;   // HTTP status of the test request, when one came back
    int ms = 0;       // time for the whole request
    net_proxy::Kind kind = net_proxy::Kind::kHttp;   // the tested proxy's kind (SS and Trojan have no answer of their own to tell errors apart)
};
/** @brief Blocking (up to about 40 s): one https request through `prefs`' proxy, whatever the
 *         switches say. Call from a task that may wait on the network. */
TestResult Test(const Prefs& prefs);
/** @brief One sentence for the user about a test result, in the UI language. */
std::string Describe(const TestResult& result);

}  // namespace valet::proxy
