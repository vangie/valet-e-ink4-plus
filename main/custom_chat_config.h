#pragma once

#include <cctype>
#include <string>

namespace custom_chat {
enum class Backend { OpenClaw = 0, Hermes = 1 };
enum class ConfigError { None, Address, PairingCode };

struct Config {
    std::string url;
    std::string code;
    Backend backend = Backend::OpenClaw;
};

inline ConfigError Validate(const Config& config) {
    const auto& url = config.url;
    const size_t prefix = url.rfind("wss://", 0) == 0 ? 6 : url.rfind("ws://", 0) == 0 ? 5 : 0;
    if (prefix == 0 || url.size() > 512) return ConfigError::Address;
    const auto authority_end = url.find_first_of("/?#", prefix);
    const auto authority = url.substr(prefix, authority_end - prefix);
    if (authority.empty() || authority.front() == ':' || authority.find('@') != std::string::npos ||
        url.find('#') != std::string::npos) return ConfigError::Address;
    for (unsigned char character : url) {
        if (std::isspace(character) || std::iscntrl(character)) return ConfigError::Address;
    }
    if (config.code.empty() || config.code.size() > 4096) return ConfigError::PairingCode;
    for (unsigned char character : config.code) {
        if (std::isspace(character) || std::iscntrl(character)) return ConfigError::PairingCode;
    }
    return ConfigError::None;
}
}
