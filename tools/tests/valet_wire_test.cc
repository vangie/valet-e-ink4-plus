#include "protocols/valet_wire.h"
#include "valet_crypto/monocypher-ed25519.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <memory>
#include <cstring>
int main(int argc, char** argv) {
    assert(argc == 3);
    std::ifstream input(argv[1]); std::string source((std::istreambuf_iterator<char>(input)), {});
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> config(cJSON_Parse(source.c_str()), cJSON_Delete);
    using valet::wire::String;
    uint8_t seed[32]; for (int i=0;i<32;++i) seed[i]=i;
    uint8_t secret[64], public_key[32]; crypto_ed25519_key_pair(secret, public_key, seed);
    auto* frame = valet::wire::Connect(secret, public_key, String(config.get(), "identity"), String(config.get(), "token"),
        cJSON_IsTrue(cJSON_GetObjectItem(config.get(), "bootstrap")), String(config.get(), "nonce"),
        cJSON_GetObjectItem(config.get(), "timestamp")->valuedouble);
    assert(frame);
    char* json = cJSON_PrintUnformatted(frame); std::ofstream(argv[2]) << json; cJSON_free(json); cJSON_Delete(frame);
    assert(valet::wire::Endpoint("wss://test.example/") == "wss://test.example/passport/v1");
    assert(valet::wire::Endpoint("wss://test.example/passport/v1") == "wss://test.example/passport/v1");
    assert(valet::wire::Endpoint("ws://test.example").empty());
    assert(valet::wire::Endpoint("wss://test.example?token=x").empty());
    assert(valet::wire::MuLaw(0) == 0xff);
    std::vector<uint8_t> audio(1600, valet::wire::MuLaw(0)); const auto wav = valet::wire::Wav(audio);
    assert(wav.size() == 1658 && !std::memcmp(wav.data(), "RIFF", 4));
    std::ofstream wavfile(std::string(argv[2]) + ".wav", std::ios::binary); wavfile.write(reinterpret_cast<const char*>(wav.data()), wav.size());
    std::vector<uint8_t> moved(480000, 0xff);
    moved.reserve(moved.size() + 58);
    const auto* allocation = moved.data();
    const auto moved_wav = valet::wire::Wav(std::move(moved));
    assert(moved_wav.data() == allocation && moved_wav.size() == 480058);
    assert(moved_wav[58] == 0xff && moved_wav.back() == 0xff);
    const auto base64 = valet::wire::Base64(wav.data(), wav.size(), true);
    assert(valet::wire::Unbase64(base64) == wav);
    std::cout << "Wire proof and 16kHz mu-law WAV generated\n";
}
