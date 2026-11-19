#include "assistant_profiles.h"
#include <cassert>
#include <iostream>
using namespace valet;
int main() {
    AssistantProfiles profiles;
    AssistantProfile home{"home", "家里的助理", {"wss://home.example/valet", "bootstrap", custom_chat::Backend::OpenClaw}, false, false, false, {}};
    AssistantProfile work{"work", "Work", {"wss://work.example/valet", "other", custom_chat::Backend::Hermes}, false, false, false, {}};
    assert(profiles.Upsert(home) && profiles.Upsert(work));
    assert(profiles.Active()->id == "home");
    assert(profiles.Select("work") && !profiles.Select("missing"));
    AssistantProfiles loaded;
    assert(DecodeProfiles(EncodeProfiles(profiles), loaded));
    assert(loaded.items.size() == 2 && loaded.Active()->id == "work");
    assert(loaded.items[0].name == "家里的助理" && !loaded.items[0].live);
    loaded.items[1].device_token = "device-token";
    loaded.items[1].connection.code.clear();
    assert(DecodeProfiles(EncodeProfiles(loaded), profiles));
    assert(profiles.items[1].device_token == "device-token" && profiles.items[1].connection.code.empty());
    auto before = EncodeProfiles(loaded);
    for (const auto& bad : {"{}", "not json", "{\"version\":2,\"items\":[]}", "{\"version\":1,\"items\":[],\"active\":\"missing\"}"}) {
        assert(!DecodeProfiles(bad, loaded)); assert(EncodeProfiles(loaded) == before);
    }
    std::string duplicate = before;
    const auto id = duplicate.find("\"id\":\"work\"");
    duplicate.replace(id, 11, "\"id\":\"home\"");
    assert(!DecodeProfiles(duplicate, loaded));
    assert(!DecodeProfiles(before + " trailing", loaded));
    assert(loaded.Remove("work") && loaded.Active()->id == "home");
    assert(loaded.Remove("home") && !loaded.Active());
    assert(DecodeProfiles(EncodeProfiles(loaded), profiles) && profiles.items.empty());
    for (size_t i = 0; i < kMaxAssistants; ++i) { home.id = "id" + std::to_string(i); assert(profiles.Upsert(home)); }
    home.id = "overflow"; assert(!profiles.Upsert(home));
    home.id = "id0"; home.name = "Renamed"; assert(profiles.Upsert(home));
    assert(profiles.items.size() == kMaxAssistants && profiles.items[0].name == "Renamed");
    auto oversized = profiles;
    for (auto& item : oversized.items) item.connection.code.assign(4096, 'x');
    assert(EncodeProfiles(oversized).empty());
    assert(!DecodeProfiles(std::string(kMaxProfilesBytes + 1, ' '), loaded));
    home.id = "bad/id"; assert(!profiles.Upsert(home));
    home.id = "valid"; home.connection.url = "https://example.com"; assert(!profiles.Upsert(home));
    std::cout << "Assistant profiles: round-trip, selection, removal, corrupt input, duplicates, limits passed\n";
}
