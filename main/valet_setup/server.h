#pragma once
#include <cstdint>
#include <string>
namespace valet {
struct SetupStatus {
    bool running = false;
    bool starting = false;   // StartSetupServer 进行中（含叫醒 Wi-Fi），此时不是失败
    bool saving = false;
    bool paired = false;     // 刚添加的助理已经拿到长期凭据
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
