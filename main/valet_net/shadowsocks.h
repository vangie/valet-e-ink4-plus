#pragma once

// Shadowsocks for the device's proxy (net_proxy Kind::kShadowsocks).
// Ciphers: AEAD (aes-128/192/256-gcm, chacha20-ietf-poly1305, xchacha20-ietf-poly1305) and the
// older stream ciphers (aes-128/192/256-cfb, aes-128/192/256-ctr, chacha20-ietf). Everything is
// built from code the firmware already links (mbedtls AES / GCM / HMAC-SHA1, monocypher ChaCha20 /
// Poly1305), so supporting them costs a few KB. SS2022 (blake3) is not supported.

#include <string>

namespace valet::ss {

/** @brief True when `method` (lower case) is a cipher this firmware can use. */
bool Supported(const std::string& method);
/** @brief Name of the index-th supported cipher, or nullptr past the end (for listing them). */
const char* MethodName(int index);
/** @brief The cipher used when none was chosen. */
constexpr char kDefaultMethod[] = "aes-256-gcm";
/** @brief Hook the implementation into net_proxy. Call once at start-up. */
void Install();

}  // namespace valet::ss
