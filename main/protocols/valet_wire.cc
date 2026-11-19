#include "valet_wire.h"
#include "valet_crypto/monocypher-ed25519.h"
#include <mbedtls/base64.h>
#include <algorithm>
#include <cstring>
#include <memory>
namespace valet::wire {
const char* String(const cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}
std::string Base64(const uint8_t* data, size_t length, bool url) {
    std::string output(4 * ((length + 2) / 3) + 1, '\0'); size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<uint8_t*>(output.data()), output.size(), &written, data, length)) return {};
    output.resize(written);
    if (url) { std::replace(output.begin(), output.end(), '+', '-'); std::replace(output.begin(), output.end(), '/', '_'); while (!output.empty() && output.back() == '=') output.pop_back(); }
    return output;
}
std::vector<uint8_t> Unbase64(std::string encoded) {
    if (encoded.size() > 660000) return {};
    std::replace(encoded.begin(), encoded.end(), '-', '+'); std::replace(encoded.begin(), encoded.end(), '_', '/');
    while (encoded.size() % 4) encoded += '=';
    std::vector<uint8_t> output(encoded.size() / 4 * 3 + 1); size_t written = 0;
    if (mbedtls_base64_decode(output.data(), output.size(), &written, reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size())) return {};
    output.resize(written); return output;
}
cJSON* Request(const char* id, const char* method) {
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req"); cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "method", method); cJSON_AddObjectToObject(root, "params"); return root;
}
cJSON* Connect(const uint8_t secret[64], const uint8_t public_key[32], const std::string& identity,
               const std::string& token, bool bootstrap, const std::string& nonce, int64_t timestamp) {
    if (nonce.empty() || nonce.size() >= 128 || token.empty() || token.size() >= 128 || token.find('|') != std::string::npos) return nullptr;
    std::string proof = "v3|" + identity + "|gateway-client|ui|operator|operator.read,operator.write,operator.talk|" +
        std::to_string(timestamp) + "|" + token + "|" + nonce + "|esp32s31|e-ink4-plus";
    uint8_t signature[64]; crypto_ed25519_sign(signature, secret, reinterpret_cast<const uint8_t*>(proof.data()), proof.size());
    auto* root = Request("valet-connect", "connect"); auto* params = cJSON_GetObjectItem(root, "params");
    cJSON_AddNumberToObject(params, "minProtocol", 3); cJSON_AddNumberToObject(params, "maxProtocol", 4);
    auto* client = cJSON_AddObjectToObject(params, "client");
    cJSON_AddStringToObject(client, "id", "gateway-client"); cJSON_AddStringToObject(client, "mode", "ui");
    cJSON_AddStringToObject(client, "displayName", "Valet E-Ink4 Plus"); cJSON_AddStringToObject(client, "version", "0.1");
    cJSON_AddStringToObject(client, "platform", "esp32s31"); cJSON_AddStringToObject(client, "deviceFamily", "e-ink4-plus");
    cJSON_AddStringToObject(params, "role", "operator");
    auto* scopes = cJSON_AddArrayToObject(params, "scopes");
    for (const auto* scope : {"operator.read", "operator.write", "operator.talk"}) cJSON_AddItemToArray(scopes, cJSON_CreateString(scope));
    auto* caps = cJSON_AddArrayToObject(params, "caps");
    for (const auto* cap : {"hello-health-on-demand", "hello-presence-self", "session-scoped-events", "passport-ima-v1", "valet-book-context-v1", "valet-ask-speech-v1"}) cJSON_AddItemToArray(caps, cJSON_CreateString(cap));
    cJSON_AddArrayToObject(params, "commands");
    auto* auth = cJSON_AddObjectToObject(params, "auth"); cJSON_AddStringToObject(auth, bootstrap ? "bootstrapToken" : "deviceToken", token.c_str());
    auto* device = cJSON_AddObjectToObject(params, "device");
    cJSON_AddStringToObject(device, "id", identity.c_str()); cJSON_AddStringToObject(device, "publicKey", Base64(public_key, 32, true).c_str());
    cJSON_AddStringToObject(device, "signature", Base64(signature, 64, true).c_str());
    cJSON_AddNumberToObject(device, "signedAt", timestamp); cJSON_AddStringToObject(device, "nonce", nonce.c_str());
    crypto_wipe(proof.data(), proof.size()); return root;
}
std::string Endpoint(std::string url) {
    if (url.rfind("wss://", 0) != 0 || url.find_first_of("?# \r\n\t") != std::string::npos) return {};
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.size() < 12 || url.substr(url.size() - 12) != "/passport/v1") url += "/passport/v1";
    return url;
}
std::string PairToken(const std::string& code) {
    auto raw = Unbase64(code.rfind("oc-pair://", 0) == 0 ? code.substr(10) : code);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithLength(reinterpret_cast<const char*>(raw.data()), raw.size()), cJSON_Delete);
    return String(json.get(), "bootstrapToken");
}
std::string MessageText(const cJSON* message) {
    const auto* content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content)) return content->valuestring;
    std::string result;
    const cJSON* item;
    cJSON_ArrayForEach(item, content) if (!std::strcmp(String(item, "type"), "text")) result += String(item, "text");
    return result.substr(0, 16384);
}
uint8_t MuLaw(int16_t sample) {
    int value = sample; const uint8_t sign = value < 0 ? 0x80 : 0;
    if (value < 0) value = -value;
    value = std::min(value, 32635) + 132;
    int exponent = 7; for (int mask = 0x4000; exponent > 0 && !(value & mask); mask >>= 1) --exponent;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | ((value >> (exponent + 3)) & 15)));
}
std::vector<uint8_t> Wav(std::vector<uint8_t> out) {
    const auto samples = out.size();
    out.insert(out.begin(), 58, 0);
    auto put = [&](size_t offset, uint32_t value, int bytes) { for (int i = 0; i < bytes; ++i) out[offset+i] = value >> (i * 8); };
    std::memcpy(out.data(), "RIFF", 4); put(4, out.size() - 8, 4); std::memcpy(out.data()+8, "WAVEfmt ", 8);
    put(16, 18, 4); put(20, 7, 2); put(22, 1, 2); put(24, 16000, 4); put(28, 16000, 4); put(32, 1, 2); put(34, 8, 2);
    std::memcpy(out.data()+38, "fact", 4); put(42, 4, 4); put(46, samples, 4); std::memcpy(out.data()+50, "data", 4); put(54, samples, 4);
    return out;
}
}
