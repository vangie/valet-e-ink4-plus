#include "assets/lang_cellular.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "wifi_board.h"
#include "application.h"
#include "display.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "sdkconfig.h"
#include "power_policy.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_station.h>

static const char *TAG = "DualNetworkBoard";

namespace {
std::atomic<DualNetworkBoard::SwitchedHook> s_switched_hook{nullptr};
constexpr uint32_t kStartStack = 6144;
}  // namespace

DualNetworkBoard::DualNetworkBoard(gpio_num_t cellular_tx_pin,
    gpio_num_t cellular_rx_pin,
    gpio_num_t cellular_dtr_pin,
    gpio_num_t cellular_ri_pin,
    int32_t default_net_type)
: Board(),
cellular_tx_pin_(cellular_tx_pin),
cellular_rx_pin_(cellular_rx_pin),
cellular_dtr_pin_(cellular_dtr_pin),
cellular_ri_pin_(cellular_ri_pin) {
    // 开机只建当前网络的板卡对象；另一个在第一次切过去时才建
    const NetworkType type = LoadNetworkTypeFromSettings(default_net_type);
    network_type_.store(type);
    ESP_LOGI(TAG, "Initialize %s board", type == NetworkType::CELLULAR ? "cellular" : "WiFi");
    active_.store(&BoardFor(type));
}

NetworkType DualNetworkBoard::LoadNetworkTypeFromSettings(int32_t default_net_type) {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    (void)default_net_type;
    return NetworkType::WIFI;
    #else
    Settings settings("network", true);
    // default_net_type: 1=4G, 0=WiFi；NVS 无记录时用该默认值
    const int fallback = (default_net_type == 0) ? 0 : 1;
    int network_type = settings.GetInt("type", fallback);
    return network_type == 1 ? NetworkType::CELLULAR : NetworkType::WIFI;
    #endif
}

void DualNetworkBoard::SaveNetworkTypeToSettings(NetworkType type) {
    Settings settings("network", true);
    int network_type = (type == NetworkType::CELLULAR) ? 1 : 0;
    settings.SetInt("type", network_type);
}

Board& DualNetworkBoard::BoardFor(NetworkType type) {
    auto& slot = type == NetworkType::CELLULAR ? cell_board_ : wifi_board_;
    if (!slot) {
        if (type == NetworkType::CELLULAR) {
            slot = std::make_unique<Nt26Board>(cellular_tx_pin_, cellular_rx_pin_, cellular_dtr_pin_, cellular_ri_pin_);
        } else {
            slot = std::make_unique<WifiBoard>();
        }
        if (network_event_callback_) slot->SetNetworkEventCallback(network_event_callback_);
    }
    return *slot;
}

void DualNetworkBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = callback;
    if (wifi_board_) wifi_board_->SetNetworkEventCallback(callback);
    if (cell_board_) cell_board_->SetNetworkEventCallback(callback);
}

void DualNetworkBoard::SetSwitchedHook(SwitchedHook hook) { s_switched_hook.store(hook); }

bool DualNetworkBoard::SwitchToNetworkType(NetworkType type) {
    #if CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    if (type != NetworkType::WIFI) {
        return false;
    }
    #endif
    std::lock_guard<std::mutex> lock(switch_mu_);
    if (type == GetNetworkType()) {
        return true;
    }
    // 传输热点（纯 AP）占着 Wi-Fi 射频：这时不动它
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode & WIFI_MODE_AP) != 0) {
        ESP_LOGW(TAG, "switch refused: the transfer hotspot holds the radio");
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    if (type == NetworkType::CELLULAR) {
        // 关掉整个 Wi-Fi 驱动（不只是暂停射频）：它的缓冲区在内部内存里，4G 下用不上
        auto& station = WifiStation::GetInstance();
        if (station.IsStarted()) station.Stop();
        auto& cell = static_cast<Nt26Board&>(BoardFor(NetworkType::CELLULAR));
        cell.OnPowerRestored();   // 允许模组再启动（上次切走时 PreparePowerOff 关了它）
        network_type_.store(type, std::memory_order_release);
        active_.store(&cell, std::memory_order_release);
    } else {
        // 停 4G 模组（UART、网卡），模组供电不动；Wi-Fi 在要用网时由 WifiBoard 自己拉起
        if (cell_board_) static_cast<Nt26Board*>(cell_board_.get())->PreparePowerOff();
        auto& wifi = BoardFor(NetworkType::WIFI);
        network_type_.store(type, std::memory_order_release);
        active_.store(&wifi, std::memory_order_release);
    }
    SaveNetworkTypeToSettings(type);
    ESP_LOGI(TAG, "switched to %s in %d ms (no restart)", type == NetworkType::CELLULAR ? "4G" : "WiFi",
             static_cast<int>((esp_timer_get_time() - t0) / 1000));
    if (const auto hook = s_switched_hook.load()) hook(type);
    PowerPolicy::GetInstance().RequestReevaluate();
    if (auto* display = GetDisplay()) display->UpdateStatusBar(true);
    // 新网络在后台连上，下一次用网不必等注册（4G 约 10–15 秒）
    if (xTaskCreate([](void* arg) {
            static_cast<DualNetworkBoard*>(arg)->Active().StartNetwork();
            vTaskDelete(nullptr);
        }, "net_start", kStartStack, this, 4, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "background network start not created; it starts on first use");
    }
    return true;
}

std::string DualNetworkBoard::GetBoardType() {
    return Active().GetBoardType();
}

void DualNetworkBoard::StartNetwork() {
    Active().StartNetwork();
}

bool DualNetworkBoard::EnsureNetworkReady(int timeout_ms) {
    return Active().EnsureNetworkReady(timeout_ms);
}

void DualNetworkBoard::RefreshNetworkWaitDeadline(int timeout_ms) {
    Active().RefreshNetworkWaitDeadline(timeout_ms);
}

void DualNetworkBoard::PauseNetworkIfNotReady() {
    Active().PauseNetworkIfNotReady();
}

bool DualNetworkBoard::IsNetworkReady() {
    return Active().IsNetworkReady();
}

bool DualNetworkBoard::IsWifiConfigMode() {
    return Active().IsWifiConfigMode();
}

NetworkInterface* DualNetworkBoard::GetNetwork() {
    return Active().GetNetwork();
}

const char* DualNetworkBoard::GetNetworkStateIcon() {
    return Active().GetNetworkStateIcon();
}

void DualNetworkBoard::SetPowerSaveMode(bool enabled) {
    Active().SetPowerSaveMode(enabled);
}

std::string DualNetworkBoard::GetBoardJson() {
    return Active().GetBoardJson();
}

std::string DualNetworkBoard::GetDeviceStatusJson() {
    return Active().GetDeviceStatusJson();
}
