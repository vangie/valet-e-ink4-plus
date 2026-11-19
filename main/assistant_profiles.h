#pragma once

#include "custom_chat_config.h"
#include <cstdint>
#include <string>
#include <vector>

namespace valet {
inline constexpr size_t kMaxAssistants = 8;
inline constexpr size_t kMaxProfilesBytes = 8192; // shared 16 KiB NVS also stores official settings
struct AssistantProfile {
    std::string id;
    std::string name;
    custom_chat::Config connection;
    // Only publish capabilities that a real handshake has verified.
    bool ask = false;
    bool live = false;
    bool context = false;
    std::string device_token;
};
struct AssistantProfiles {
    std::vector<AssistantProfile> items;
    std::string active_id;
    uint64_t revision = 0; // In-memory optimistic lock; never serialized.
    const AssistantProfile* Active() const;
    bool Upsert(const AssistantProfile& profile);
    bool Select(const std::string& id);
    bool Remove(const std::string& id);
};
bool ValidProfile(const AssistantProfile& profile);
std::string EncodeProfiles(const AssistantProfiles& profiles);
// Invalid/corrupt input leaves the destination unchanged.
bool DecodeProfiles(const std::string& json, AssistantProfiles& profiles);

// Startup and storage-domain task only. UI reads a copied in-memory snapshot.
void InitializeAssistantProfiles();
AssistantProfiles LoadAssistantProfiles();
bool SaveAssistantProfiles(const AssistantProfiles& profiles);
} // namespace valet
