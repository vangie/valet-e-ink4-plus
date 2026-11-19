#pragma once
#include <cstdint>
#include <string>
namespace valet {
struct SetupStatus {
    bool running = false;
    bool saving = false;
    uint32_t generation = 0;
    std::string url;
    std::string note;
};
// Network task: uses existing Wi-Fi STA, never creates a hotspot or changes network mode.
bool StartSetupServer();
void StopSetupServer();
// Invalidates the access session immediately; schedules network cleanup off UI thread.
void CloseSetupSession();
SetupStatus GetSetupStatus();
}
