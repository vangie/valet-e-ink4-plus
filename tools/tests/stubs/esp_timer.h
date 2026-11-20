#pragma once
#include <chrono>
#include <cstdint>
// Host builds: microseconds on a monotonic clock, like the firmware's time since boot.
inline int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
