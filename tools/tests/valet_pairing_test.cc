#include "valet_setup/pairing.h"
#include <mbedtls/base64.h>
#include <cassert>
#include <cJSON.h>
#include <iostream>
#include <algorithm>
std::string Code(const std::string& url, const std::string& token = "bootstrap-test") {
    const auto json = "{\"url\":\"" + url + "\",\"bootstrapToken\":\"" + token + "\"}";
    unsigned char encoded[2048]; size_t length;
    assert(mbedtls_base64_encode(encoded, sizeof(encoded), &length,
        reinterpret_cast<const unsigned char*>(json.data()), json.size()) == 0);
    std::string result(reinterpret_cast<char*>(encoded), length);
    std::replace(result.begin(), result.end(), '+', '-'); std::replace(result.begin(), result.end(), '/', '_');
    while (result.back() == '=') result.pop_back();
    return result;
}
std::string Payload(const std::string& url, const std::string& code, const char* backend = "openclaw") {
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", "new"); cJSON_AddStringToObject(root, "name", "家里的助理");
    cJSON_AddStringToObject(root, "backend", backend);
    cJSON_AddStringToObject(root, "url", url.c_str()); cJSON_AddStringToObject(root, "code", code.c_str());
    char* json = cJSON_PrintUnformatted(root); std::string result(json);
    cJSON_free(json); cJSON_Delete(root); return result;
}
int main() {
    valet::AssistantProfile p;
    assert(!valet::ParsePairing(Payload("wss://gateway.example/", Code("wss://gateway.example")), p));
    assert(p.name == "家里的助理" && !p.ask && !p.live && !p.context);
    assert(!valet::ParsePairing(Payload("", "oc-pair://" + Code("wss://hermes.example"), "hermes"), p));
    assert(p.connection.backend == custom_chat::Backend::Hermes);
    const auto prior = p.connection.url;
    assert(valet::ParsePairing(Payload("wss://other.example", Code("wss://gateway.example")), p));
    assert(p.connection.url == prior);
    assert(valet::ParsePairing(Payload("ws://gateway.example", Code("ws://gateway.example")), p));
    assert(valet::ParsePairing(Payload("wss://user:password@example.com", Code("wss://user:password@example.com")), p));
    assert(valet::ParsePairing(Payload("wss://gateway.example", "truncated"), p));
    assert(valet::ParsePairing(Payload("wss://gateway.example", Code("wss://gateway.example", "")), p));
    assert(valet::ParsePairing(Payload("wss://gateway.example", Code("wss://gateway.example"), "invented"), p));
    assert(valet::ParsePairing(std::string(6145, ' '), p));
    assert(valet::ParsePairing("{}", p));
    assert(valet::ParsePairing(Payload("wss://gateway.example", Code("wss://gateway.example")) + "trailing", p));
    std::cout << "Pairing parser: real base64url, oc-pair, address binding, no verified capabilities, invalid codes passed\n";
}
