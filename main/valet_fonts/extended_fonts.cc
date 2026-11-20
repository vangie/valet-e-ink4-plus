#include "extended_fonts.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dual_network_board.h"
#include "http.h"
#include "power_policy.h"
#include "sd_access.h"
#include "sd_paths.h"
#include "SdCardManager.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <psa/crypto.h>
#include <wifi_station.h>

namespace valet::fonts {
namespace {
constexpr char TAG[] = "ExtFonts";

// The pack published with the Valet firmware. Size and digest are those in
// tools/fontpack/fonts/Extended-Fonts/manifest.json; a file that differs is not installed.
constexpr char kUrl[] =
    "https://github.com/vangie/valet-e-ink4-plus/releases/download/fonts-1/extended-fonts.fontpack";
constexpr uint64_t kBytes = 267832481ull;
constexpr char kSha256[] = "7cad7820f4a304535392d118e7b9c01e6ed02c797376d82278afb0f4a4489de6";
constexpr char kPartPath[] = SD_PATH_FONT_EXT ".part";
constexpr size_t kIoBytes = 64 * 1024;
constexpr int kTimeoutMs = 20000;
constexpr int kMaxRedirects = 4;
constexpr int kMaxAttempts = 6;   // reconnect-and-resume attempts in one run

std::mutex s_mutex;
Status s_status;
std::atomic<bool> s_running{false};
std::atomic<bool> s_cancel{false};
std::atomic<bool> s_refreshing{false};

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}
void Set(State state, int percent = 0, uint64_t done = 0, const char* message = "") {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status.state = state;
    s_status.percent = percent;
    s_status.done_bytes = done;
    std::snprintf(s_status.message, sizeof(s_status.message), "%s", message);
}
uint64_t FileSize(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}
// Wi-Fi only: the pack is 255 MB and the bundled SIM is metered.
bool OnWifi() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    const bool wifi = dual ? dual->GetNetworkType() == NetworkType::WIFI : true;
    return wifi && WifiStation::GetInstance().IsConnected();
}

// GitHub answers a release download with a redirect to a signed address, and the
// device's HTTP client does not follow redirects. Open `url` with an optional
// range, following redirects; on success `http` is positioned at the body.
bool OpenFollowing(std::unique_ptr<Http>& http, std::string url, uint64_t from, int* status_out) {
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        auto network = Board::GetInstance().GetNetwork();
        if (!network) return false;
        http = network->CreateHttp(0);
        if (!http) return false;
        http->SetTimeout(kTimeoutMs);
        http->SetHeader("Accept", "*/*");
        http->SetHeader("User-Agent", "valet-e-ink");
        http->SetHeader("Connection", "close");
        if (from > 0) {
            char range[48];
            std::snprintf(range, sizeof(range), "bytes=%llu-", static_cast<unsigned long long>(from));
            http->SetHeader("Range", range);
        }
        if (!http->Open("GET", url)) return false;
        const int status = http->GetStatusCode();
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            std::string next = http->GetResponseHeader("Location");
            if (next.empty()) next = http->GetResponseHeader("location");
            http->Close();
            // Only follow to another https address; never downgrade.
            if (next.rfind("https://", 0) != 0) return false;
            url = std::move(next);
            continue;
        }
        *status_out = status;
        return true;
    }
    return false;
}

enum class Fetch { kDone, kRetry, kStop, kFatal };
// One connection: append to the part file from its current size.
Fetch FetchOnce(uint8_t* buffer, std::string& error) {
    const uint64_t have = FileSize(kPartPath);
    if (have > kBytes) { unlink(kPartPath); return Fetch::kRetry; }
    if (have == kBytes) return Fetch::kDone;
    std::unique_ptr<Http> http;
    int status = 0;
    if (!OpenFollowing(http, kUrl, have, &status)) { error = Text("连不上下载地址", "Cannot reach the download"); return Fetch::kRetry; }
    if (status == 200 && have > 0) {
        // The server ignored the range; start over rather than append to a prefix.
        http->Close();
        unlink(kPartPath);
        return Fetch::kRetry;
    }
    if (status != 200 && status != 206) {
        http->Close();
        ESP_LOGW(TAG, "download status=%d", status);
        error = status == 404 ? Text("字库文件不在下载地址上", "The pack is not at the download address")
                              : Text("下载地址拒绝了请求", "The download was refused");
        return status == 404 || status == 403 ? Fetch::kFatal : Fetch::kRetry;
    }
    FILE* file = fopen(kPartPath, have > 0 ? "ab" : "wb");
    if (!file) { http->Close(); error = Text("SD 卡写不进去", "Cannot write to the SD card"); return Fetch::kFatal; }
    uint64_t total = have;
    Fetch result = Fetch::kRetry;
    int last_percent = -1;
    for (;;) {
        if (s_cancel.load()) { result = Fetch::kStop; break; }
        if (!OnWifi()) { error = Text("Wi-Fi 断了", "Wi-Fi dropped"); result = Fetch::kRetry; break; }
        const int n = http->Read(reinterpret_cast<char*>(buffer), kIoBytes);
        if (n < 0) { error = Text("下载中断了", "The download was interrupted"); result = Fetch::kRetry; break; }
        if (n == 0) { result = total == kBytes ? Fetch::kDone : Fetch::kRetry; break; }
        if (total + static_cast<uint64_t>(n) > kBytes) { error = Text("下载到的文件不对", "The downloaded file is wrong"); result = Fetch::kFatal; break; }
        if (fwrite(buffer, 1, static_cast<size_t>(n), file) != static_cast<size_t>(n)) {
            error = Text("SD 卡写不进去，可能满了", "Cannot write to the SD card; it may be full");
            result = Fetch::kFatal;
            break;
        }
        total += static_cast<uint64_t>(n);
        const int percent = static_cast<int>(total * 100 / kBytes);
        if (percent != last_percent) { last_percent = percent; Set(State::kDownloading, percent, total); }
    }
    http->Close();
    if (fflush(file) != 0 || fclose(file) != 0) {
        if (result == Fetch::kDone) { error = Text("SD 卡写不进去", "Cannot write to the SD card"); result = Fetch::kFatal; }
    }
    return result;
}

bool Verify(uint8_t* buffer) {
    FILE* file = fopen(kPartPath, "rb");
    if (!file) return false;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    uint64_t total = 0;
    while (ok) {
        if (s_cancel.load()) { ok = false; break; }
        const size_t n = fread(buffer, 1, kIoBytes, file);
        if (n == 0) break;
        ok = psa_hash_update(&op, buffer, n) == PSA_SUCCESS;
        total += n;
        Set(State::kVerifying, static_cast<int>(total * 100 / kBytes), total);
    }
    fclose(file);
    uint8_t digest[32];
    size_t length = 0;
    if (!ok || total != kBytes || psa_hash_finish(&op, digest, sizeof(digest), &length) != PSA_SUCCESS || length != 32) {
        psa_hash_abort(&op);
        return false;
    }
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::strcmp(hex, kSha256) == 0;
}

void Worker(void*) {
    // Keep the device awake and the network up for the whole run.
    PowerNeedHold hold(PowerNeed::OtaDownload);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> buffer(
        static_cast<uint8_t*>(heap_caps_malloc(kIoBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
    std::string error;
    Fetch result = Fetch::kFatal;
    if (!buffer) error = Text("内存不够", "Out of memory");
    else {
        sd_access::Transaction transaction;
        if (!transaction) error = Text("SD 卡现在用不了", "The SD card is not available");
        else {
            const uint64_t resumed = FileSize(kPartPath);
            Set(State::kDownloading, static_cast<int>(resumed * 100 / kBytes), resumed);
            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                const uint64_t before = FileSize(kPartPath);
                result = FetchOnce(buffer.get(), error);
                if (result != Fetch::kRetry) break;
                if (s_cancel.load()) { result = Fetch::kStop; break; }
                // Progress since the last attempt earns the attempt back.
                if (FileSize(kPartPath) > before) attempt = -1;
                ESP_LOGW(TAG, "retry at %llu bytes: %s", static_cast<unsigned long long>(FileSize(kPartPath)), error.c_str());
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            if (result == Fetch::kDone) {
                Set(State::kVerifying, 0, 0);
                if (!Verify(buffer.get())) {
                    // A wrong file is useless as a resume point; drop it.
                    if (!s_cancel.load()) unlink(kPartPath);
                    error = Text("校验没通过，已删除，请重新下载", "The check failed; the file was removed. Download again.");
                    result = s_cancel.load() ? Fetch::kStop : Fetch::kFatal;
                } else {
                    unlink(SD_PATH_FONT_EXT);
                    if (rename(kPartPath, SD_PATH_FONT_EXT) != 0) {
                        error = Text("装不上：SD 卡写不进去", "Cannot install: the SD card is not writable");
                        result = Fetch::kFatal;
                    }
                }
            }
        }
    }
    if (result == Fetch::kDone) {
        ESP_LOGI(TAG, "installed %s", SD_PATH_FONT_EXT);
        Set(State::kInstalled, 100, kBytes);
    } else if (result == Fetch::kStop) {
        const uint64_t kept = FileSize(kPartPath);
        Set(State::kMissing, static_cast<int>(kept * 100 / kBytes), kept);
    } else {
        if (error.empty()) error = Text("没下载完，可以接着下", "Not finished; it can be resumed");
        ESP_LOGW(TAG, "failed: %s", error.c_str());
        const uint64_t kept = FileSize(kPartPath);
        Set(State::kFailed, static_cast<int>(kept * 100 / kBytes), kept, error.c_str());
    }
    s_running.store(false);
    vTaskDelete(nullptr);
}

void RefreshTask(void*) {
    {
        sd_access::Transaction transaction;
        if (transaction && !s_running.load()) {
            const bool installed = FileSize(SD_PATH_FONT_EXT) == kBytes;
            const uint64_t part = installed ? 0 : FileSize(kPartPath);
            std::lock_guard<std::mutex> lock(s_mutex);
            // Leave a failure message in place until the next attempt.
            if (installed) { s_status = Status{}; s_status.state = State::kInstalled; s_status.percent = 100; s_status.done_bytes = kBytes; }
            else if (s_status.state != State::kFailed) {
                s_status = Status{};
                s_status.percent = static_cast<int>(part * 100 / kBytes);
                s_status.done_bytes = part;
            }
        }
    }
    s_refreshing.store(false);
    vTaskDelete(nullptr);
}
}  // namespace

uint64_t PackBytes() { return kBytes; }

Status GetStatus() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_status;
}

void Refresh() {
    if (s_running.load() || s_refreshing.exchange(true)) return;
    if (xTaskCreate(RefreshTask, "extfont_stat", 4096, nullptr, 3, nullptr) != pdPASS) s_refreshing.store(false);
}

bool Start() {
    if (s_running.load()) return false;
    if (!OnWifi()) {
        Set(State::kFailed, 0, 0, Text("要连上 Wi-Fi 才能下载（255 MB）", "Connect to Wi-Fi to download (255 MB)"));
        return false;
    }
    if (!SdCardManager::GetInstance().IsMounted()) {
        Set(State::kFailed, 0, 0, Text("没有 SD 卡", "No SD card"));
        return false;
    }
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) == ESP_OK &&
        free_bytes + FileSize(kPartPath) < kBytes + (8ull << 20)) {
        Set(State::kFailed, 0, 0, Text("SD 卡空间不够，要 256 MB", "Not enough room on the SD card (256 MB needed)"));
        return false;
    }
    if (s_running.exchange(true)) return false;
    s_cancel.store(false);
    Set(State::kDownloading, 0, 0);
    // The stack must be internal RAM: the task writes to the card and the flash cache is involved.
    if (xTaskCreate(Worker, "extfont_dl", 8192, nullptr, 4, nullptr) != pdPASS) {
        s_running.store(false);
        Set(State::kFailed, 0, 0, Text("启动不了下载", "Could not start the download"));
        return false;
    }
    return true;
}

void Cancel() { s_cancel.store(true); }

}  // namespace valet::fonts
