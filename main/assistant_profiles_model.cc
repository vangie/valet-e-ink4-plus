#include "assistant_profiles.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <cJSON.h>

namespace valet {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string Field(const cJSON* node, const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(node, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
bool Flag(const cJSON* node, const char* name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(node, name));
}
}
const AssistantProfile* AssistantProfiles::Active() const {
    for (const auto& item : items) if (item.id == active_id) return &item;
    return nullptr;
}
bool ValidProfile(const AssistantProfile& p) {
    if (p.id.empty() || p.id.size() > 48 || p.name.empty() || p.name.size() > 96) return false;
    for (unsigned char c : p.id) if (!std::isalnum(c) && c != '-' && c != '_') return false;
    for (unsigned char c : p.name) if (std::iscntrl(c)) return false;
    if (p.connection.backend != custom_chat::Backend::OpenClaw &&
        p.connection.backend != custom_chat::Backend::Hermes) return false;
    if (p.device_token.size() > 127) return false;
    if (!p.device_token.empty()) {
        auto config = p.connection; config.code = "paired";
        return custom_chat::Validate(config) == custom_chat::ConfigError::None;
    }
    return custom_chat::Validate(p.connection) == custom_chat::ConfigError::None;
}
bool AssistantProfiles::Upsert(const AssistantProfile& p) {
    if (!ValidProfile(p)) return false;
    for (auto& item : items) if (item.id == p.id) { item = p; return true; }
    if (items.size() >= kMaxAssistants) return false;
    items.push_back(p);
    if (active_id.empty()) active_id = p.id;
    return true;
}
bool AssistantProfiles::Select(const std::string& id) {
    for (const auto& p : items) if (p.id == id) { active_id = id; return true; }
    return false;
}
bool AssistantProfiles::Remove(const std::string& id) {
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& p) { return p.id == id; });
    if (found == items.end()) return false;
    items.erase(found);
    if (active_id == id) active_id = items.empty() ? "" : items.front().id;
    return true;
}
std::string EncodeProfiles(const AssistantProfiles& profiles) {
    if (profiles.items.size() > kMaxAssistants || (!profiles.items.empty() && !profiles.Active()) ||
        (profiles.items.empty() && !profiles.active_id.empty())) return {};
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root) return {};
    cJSON_AddNumberToObject(root.get(), "version", 1);
    cJSON_AddStringToObject(root.get(), "active", profiles.active_id.c_str());
    auto* items = cJSON_AddArrayToObject(root.get(), "items");
    if (!items) return {};
    std::vector<std::string> ids;
    for (const auto& p : profiles.items) {
        if (!ValidProfile(p) || std::find(ids.begin(), ids.end(), p.id) != ids.end()) return {};
        ids.push_back(p.id);
        auto* item = cJSON_CreateObject();
        if (!item) return {};
        cJSON_AddItemToArray(items, item);
        cJSON_AddStringToObject(item, "id", p.id.c_str());
        cJSON_AddStringToObject(item, "name", p.name.c_str());
        cJSON_AddStringToObject(item, "url", p.connection.url.c_str());
        cJSON_AddStringToObject(item, "code", p.connection.code.c_str());
        cJSON_AddStringToObject(item, "backend", p.connection.backend == custom_chat::Backend::Hermes ? "hermes" : "openclaw");
        cJSON_AddBoolToObject(item, "ask", p.ask);
        cJSON_AddBoolToObject(item, "live", p.live);
        cJSON_AddBoolToObject(item, "context", p.context);
        cJSON_AddStringToObject(item, "device_token", p.device_token.c_str());
    }
    char* output = cJSON_PrintUnformatted(root.get());
    if (!output) return {};
    std::string result(output);
    cJSON_free(output);
    if (result.size() > kMaxProfilesBytes) return {};
    return result;
}
bool DecodeProfiles(const std::string& input, AssistantProfiles& result) {
    if (input.empty() || input.size() > kMaxProfilesBytes || input.find('\0') != std::string::npos) return false;
    Json root(cJSON_ParseWithOpts(input.c_str(), nullptr, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) return false;
    auto* version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    auto* items = cJSON_GetObjectItemCaseSensitive(root.get(), "items");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsArray(items) ||
        cJSON_GetArraySize(items) > static_cast<int>(kMaxAssistants)) return false;
    AssistantProfiles parsed;
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        const auto backend = Field(item, "backend");
        if (backend != "openclaw" && backend != "hermes") return false;
        AssistantProfile p;
        p.id = Field(item, "id"); p.name = Field(item, "name");
        p.connection = {Field(item, "url"), Field(item, "code"), backend == "hermes" ? custom_chat::Backend::Hermes : custom_chat::Backend::OpenClaw};
        p.device_token = Field(item, "device_token");
        p.ask = Flag(item, "ask"); p.live = Flag(item, "live"); p.context = Flag(item, "context");
        for (const auto& prior : parsed.items) if (prior.id == p.id) return false;
        if (!parsed.Upsert(p)) return false;
    }
    parsed.active_id = Field(root.get(), "active");
    if ((!parsed.items.empty() && !parsed.Active()) || (parsed.items.empty() && !parsed.active_id.empty())) return false;
    result = std::move(parsed);
    return true;
}
} // namespace valet
