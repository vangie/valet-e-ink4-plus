#pragma once

// Trojan for the device's proxy (net_proxy Kind::kTrojan): a TLS connection to the server, then
// hex(SHA-224(password)) CRLF, CONNECT, the target in SOCKS5 form, CRLF, and the data both ways.
// Plain TCP only (no WebSocket or gRPC transport). The server name sent in TLS is Config::sni, or
// the server's address; Config::insecure skips the certificate check, as Surge's skip-cert-verify.
// TLS comes from the mbedtls the firmware already links, its buffers in PSRAM.

namespace valet::trojan {

/** @brief Hook the implementation into net_proxy. Call once at start-up. */
void Install();

}  // namespace valet::trojan
