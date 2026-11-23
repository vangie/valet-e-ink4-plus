#include "auto_link.h"

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "power_policy.h"
#include "protocols/valet_protocol.h"
#include "standby_screen/standby_screen.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>
#include <wifi_station.h>

namespace valet::autolink {
namespace {
constexpr char TAG[] = "AutoLink";
constexpr uint32_t kMiss = 1;    // Wi-Fi 找不到存过的网络
constexpr uint32_t kProbe = 2;   // 4G 刚醒要用网：看看有没有存过的 Wi-Fi
constexpr uint32_t kForce = 4;   // 测试：马上扫，不管间隔
constexpr int kPeriodMs = 3 * 60 * 1000;          // 在 4G 上空闲时多久扫一次
constexpr int64_t kProbeGapUs = 60LL * 1000000;   // 两次扫描至少隔这么久
constexpr int64_t kAfterMissUs = 5LL * 60 * 1000000;   // 因为找不到 Wi-Fi 换到 4G 后，这么久内不换回去（防来回跳）
constexpr int kMinRssi = -75;    // 太弱的 Wi-Fi 不换过去
// 扫描时 Wi-Fi 驱动临时占约 35 KB 内部内存（实测 116 KB -> 82 KB）。空闲不到这么多就不扫，免得掉到安全线（50 KB）下面。
constexpr size_t kMinFreeToScan = 90 * 1024;
constexpr int kAfterCallPollMs = 5000;   // 通话中该看没看的，挂断后多快补上
constexpr uint32_t kStack = 6144;

TaskHandle_t s_task = nullptr;
bool s_after_call = false;   // 通话中错过了一次扫描：挂断后补
int64_t s_last_scan_us = 0;
int64_t s_quiet_until_us = 0;

DualNetworkBoard* Dual() { return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance()); }

void Notify(uint32_t bit) {
    if (s_task) xTaskNotify(s_task, bit, eSetBits);
}

// 正在提问、通话或下载：这时网好好的，不去换它
bool Busy() {
    const auto state = Application::GetInstance().GetDeviceState();
    return ValetProtocol::Live() || state == kDeviceStateListening || state == kDeviceStateSpeaking ||
           PowerPolicy::GetInstance().IsHeld(PowerNeed::OtaDownload);
}

// 临时起 Wi-Fi 驱动扫一次，看有没有存过的网络。驱动被别人占着（网络页、传输热点）就不扫。
bool ScanForSaved(std::string* ssid, int* rssi) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK) return false;
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != nullptr) return false;
    const auto saved = SsidManager::GetInstance().GetSsidList();
    if (saved.empty()) return false;
    const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (heap_before < kMinFreeToScan) {
        ESP_LOGI(TAG, "scan skipped: %u bytes of internal RAM free", unsigned(heap_before));
        return false;
    }
    size_t heap_low = heap_before;
    const int64_t t0 = esp_timer_get_time();
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    if (!netif) return false;
    bool found = false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    if (esp_wifi_init(&cfg) == ESP_OK) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK) {
            heap_low = std::min(heap_low, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            wifi_scan_config_t scan{};
            if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
                uint16_t count = 0;
                esp_wifi_scan_get_ap_num(&count);
                count = std::min<uint16_t>(count, 24);
                auto* records = static_cast<wifi_ap_record_t*>(
                    heap_caps_malloc(sizeof(wifi_ap_record_t) * std::max<uint16_t>(count, 1), MALLOC_CAP_SPIRAM));
                if (records) {
                    esp_wifi_scan_get_ap_records(&count, records);
                    for (int i = 0; i < count; ++i) {
                        const char* name = reinterpret_cast<const char*>(records[i].ssid);
                        const bool known = !WifiStation::TestBlind() && std::any_of(saved.begin(), saved.end(), [&](const SsidItem& item) { return item.ssid == name; });
                        if (known && (!found || records[i].rssi > *rssi)) {
                            found = true;
                            *ssid = name;
                            *rssi = records[i].rssi;
                        }
                    }
                    heap_caps_free(records);
                } else {
                    esp_wifi_clear_ap_list();
                }
            }
            esp_wifi_stop();
        }
        esp_wifi_deinit();
    }
    esp_netif_destroy_default_wifi(netif);
    ESP_LOGI(TAG, "scan on 4G: %s in %d ms, internal RAM %u free before, %u at the lowest, %u after",
             found ? "saved network seen" : "no saved network", int((esp_timer_get_time() - t0) / 1000),
             unsigned(heap_before), unsigned(heap_low), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    return found;
}

void Task(void*) {
    while (true) {
        uint32_t bits = 0;
        const bool timed_out = xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(s_after_call ? kAfterCallPollMs : kPeriodMs)) != pdTRUE;
        if (timed_out && s_after_call && !ValetProtocol::Live()) {
            s_after_call = false;
            bits |= kForce;   // 挂断了：马上看看
        }
        auto* dual = Dual();
        if (!dual || dual->GetNetworkMode() != NetworkMode::AUTO) continue;
        const int64_t now = esp_timer_get_time();

        if (bits & kMiss) {
            if (dual->GetNetworkType() != NetworkType::WIFI) continue;
            ESP_LOGI(TAG, "no saved Wi-Fi network reachable -> 4G");
            if (dual->SwitchToNetworkType(NetworkType::CELLULAR)) s_quiet_until_us = esp_timer_get_time() + kAfterMissUs;
            continue;
        }

        // 在 4G 上：要不要看看有没有存过的 Wi-Fi
        if (dual->GetNetworkType() != NetworkType::CELLULAR || StandbyScreen::IsActive()) continue;
        const bool force = (bits & kForce) != 0;
        const bool probe = (bits & (kProbe | kForce)) != 0;
        if (ValetProtocol::Live()) {                  // 通话中绝不换：挂断后再看
            s_after_call = true;
            continue;
        }
        if (!probe && Busy()) continue;               // 定时扫：忙就下次
        if (!force && now < s_quiet_until_us) continue;
        if (!force && now - s_last_scan_us < kProbeGapUs) continue;
        s_last_scan_us = now;
        std::string ssid;
        int rssi = -127;
        if (!ScanForSaved(&ssid, &rssi)) continue;
        if (rssi < kMinRssi) {
            ESP_LOGI(TAG, "saved network too weak (%d dBm), staying on 4G", rssi);
            continue;
        }
        if (ValetProtocol::Live()) {                  // 扫的时候来了通话
            s_after_call = true;
            continue;
        }
        if (!probe && Busy()) continue;
        ESP_LOGI(TAG, "saved network %s at %d dBm -> Wi-Fi", ssid.c_str(), rssi);
        dual->SwitchToNetworkType(NetworkType::WIFI);
    }
}
}  // namespace

void ProbeNow() { Notify(kForce); }

void Init() {
    if (s_task || !Dual()) return;
    WifiStation::GetInstance().OnNoSavedNetwork([] { Notify(kMiss); });
    DualNetworkBoard::SetWakeHook([] { Notify(kProbe); });
    // 内部内存的栈：扫描会初始化 Wi-Fi 驱动，切换会写 NVS
    if (xTaskCreate(Task, "auto_link", kStack, nullptr, 3, &s_task) != pdPASS) {
        s_task = nullptr;
        ESP_LOGE(TAG, "task not created; automatic switching is off");
        return;
    }
    // No saved network, or Wi-Fi switched off: Wi-Fi never scans, so it never reports a miss. Use 4G.
    if (SsidManager::GetInstance().GetSsidList().empty() || !WifiStation::GetInstance().IsUserEnabled()) Notify(kMiss);
}

}  // namespace valet::autolink
