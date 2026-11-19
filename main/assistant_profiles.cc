#include "assistant_profiles.h"
#include "settings.h"
#include <mutex>

namespace valet {
namespace {
std::mutex s_profiles_mutex;
AssistantProfiles s_profiles;
}
void InitializeAssistantProfiles() {
    Settings settings("chat_cloud");
    AssistantProfiles loaded;
    auto json = settings.GetBlob("assistants_v1", kMaxProfilesBytes);
    if (json.empty()) json = settings.GetString("assistants_v1");
    if (!json.empty()) DecodeProfiles(json, loaded);
    else {
        // Preserve the previously saved single gateway as an unverified connection.
        AssistantProfile legacy;
        legacy.id = "legacy"; legacy.name = "Imported assistant";
        legacy.connection.url = settings.GetString("agent_url");
        legacy.connection.code = settings.GetString("agent_code");
        legacy.connection.backend = settings.GetInt("agent_backend", 0) == 1
            ? custom_chat::Backend::Hermes : custom_chat::Backend::OpenClaw;
        loaded.Upsert(legacy);
    }
    std::lock_guard<std::mutex> lock(s_profiles_mutex);
    s_profiles = std::move(loaded);
}
AssistantProfiles LoadAssistantProfiles() {
    std::lock_guard<std::mutex> lock(s_profiles_mutex);
    return s_profiles;
}
bool SaveAssistantProfiles(const AssistantProfiles& profiles) {
    // Serialize persistence and reject stale snapshots, including paired credentials.
    std::lock_guard<std::mutex> lock(s_profiles_mutex);
    if (profiles.revision != s_profiles.revision) return false;
    const auto json = EncodeProfiles(profiles);
    if (json.empty()) return false;
    // A blob can span NVS pages. An NVS string is limited to 4000 bytes,
    // which can reject a valid list of assistants with longer pairing codes.
    Settings settings("chat_cloud", true);
    if (!settings.SetBlob("assistants_v1", json) || !settings.Commit()) return false;
    s_profiles = profiles;
    ++s_profiles.revision;
    return true;
}
} // namespace valet
