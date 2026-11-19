#pragma once
#include <cJSON.h>
#include <cstdint>
#include <string>
#include <vector>
namespace valet::wire {
const char* String(const cJSON* object, const char* key);
std::string Base64(const uint8_t* data, size_t length, bool url = false);
std::vector<uint8_t> Unbase64(std::string encoded);
cJSON* Request(const char* id, const char* method);
// Signed protocol v3 identity: same scope order and fields as Passport.
cJSON* Connect(const uint8_t secret[64], const uint8_t public_key[32], const std::string& identity,
               const std::string& token, bool bootstrap, const std::string& nonce, int64_t timestamp);
std::string Endpoint(std::string url);
std::string PairToken(const std::string& code);
std::string MessageText(const cJSON* message);
uint8_t MuLaw(int16_t sample);
std::vector<uint8_t> Wav(std::vector<uint8_t> mulaw);
}
