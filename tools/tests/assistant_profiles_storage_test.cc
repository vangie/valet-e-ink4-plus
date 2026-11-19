#include "assistant_profiles.h"
#include "settings.h"
#include <cassert>
#include <iostream>
#include <string_view>
#include <thread>
#include <atomic>

// Fake only the persistence boundary; exercise the firmware's actual cache/save code.
namespace {
std::string persisted;
std::string legacy;
bool fail_commit = false;
}
Settings::Settings(const std::string& ns, bool writable) : ns_(ns), read_write_(writable) {}
Settings::~Settings() = default;
std::string Settings::GetString(const std::string& key, const std::string& fallback) {
    return key == "assistants_v1" && persisted.empty() && !legacy.empty() ? legacy : fallback;
}
int32_t Settings::GetInt(const std::string&, int32_t fallback) { return fallback; }
std::string Settings::GetBlob(const char* key, size_t limit) {
    assert(std::string_view(key) == "assistants_v1");
    return persisted.size() <= limit ? persisted : std::string();
}
bool Settings::SetBlob(const char* key, const std::string& value) {
    assert(std::string_view(key) == "assistants_v1");
    if (!fail_commit) persisted = value;
    return true;
}
bool Settings::Commit() { return !fail_commit; }
int main() {
    using namespace valet;
    AssistantProfiles initial;
    AssistantProfile home{"home", "Home", {"wss://home.example/", "code", custom_chat::Backend::OpenClaw}, false, false, false, {}};
    assert(initial.Upsert(home));
    legacy = EncodeProfiles(initial);
    InitializeAssistantProfiles();
    assert(LoadAssistantProfiles().Active()->id == "home");
    auto stale_ui = LoadAssistantProfiles();
    auto paired = LoadAssistantProfiles();
    paired.items[0].device_token = "paired-token";
    paired.items[0].connection.code.clear();
    assert(SaveAssistantProfiles(paired));
    stale_ui.items[0].name = "Stale rename";
    assert(!SaveAssistantProfiles(stale_ui));
    assert(LoadAssistantProfiles().items[0].device_token == "paired-token");
    assert(persisted.find("paired-token") != std::string::npos);
    auto failed = LoadAssistantProfiles();
    failed.items[0].name = "Failed write";
    fail_commit = true;
    assert(!SaveAssistantProfiles(failed));
    assert(LoadAssistantProfiles().revision == failed.revision);
    assert(LoadAssistantProfiles().items[0].name == "Home");
    fail_commit = false;
    // Two writers with the same snapshot must not both overwrite persistence.
    const auto shared = LoadAssistantProfiles();
    std::atomic<int> ready{0}, successes{0};
    auto write = [&](const char* name) {
        auto candidate = shared;
        candidate.items[0].name = name;
        ready.fetch_add(1);
        while (ready.load() != 2) std::this_thread::yield();
        if (SaveAssistantProfiles(candidate)) ++successes;
    };
    std::thread a(write, "Writer A"), b(write, "Writer B");
    a.join(); b.join();
    assert(successes == 1);
    AssistantProfiles stored;
    assert(DecodeProfiles(persisted, stored));
    assert(stored.items[0].name == LoadAssistantProfiles().items[0].name);
    assert(stored.items[0].device_token == "paired-token");
    auto larger = LoadAssistantProfiles();
    for (int i = 0; i < 4; ++i) {
        auto unpaired = home;
        unpaired.id = "unpaired-" + std::to_string(i);
        unpaired.connection.code.assign(1100, 'x');
        assert(larger.Upsert(unpaired));
    }
    assert(SaveAssistantProfiles(larger) && persisted.size() > 4000);
    InitializeAssistantProfiles();
    assert(LoadAssistantProfiles().items.size() == 5);
    std::cout << "Profile storage: legacy read, large blobs, stale write, failed commit, concurrent writers PASS\n";
}
