#include "dev_link.h"

#include "sdkconfig.h"

#if CONFIG_VALET_DEV_LINK

#include "config.h"
#include "display_orient.h"
#include "epd_refresh.h"
#include "epdiy.h"
#include "lv_adapter_epdiy.h"
#include "metalio_keys.h"
#include "metalio_touch.h"
#include "screen_common.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

const char* TAG = "ValetDev";

constexpr int kPanelW = DISPLAY_WIDTH;   // portrait
constexpr int kPanelH = DISPLAY_HEIGHT;
constexpr int kStepUs = 10000;
constexpr size_t kChunk = 540;           // raw bytes per screenshot line (720 base64 chars)

// One line at a time under the stdout lock, so a frame never splits a log line
// in the middle of our own text. Returns false when the host is not reading.
bool WriteLine(const char* text, size_t length) {
    flockfile(stdout);
    bool ok = usb_serial_jtag_write_bytes("\r\n", 2, pdMS_TO_TICKS(300)) == 2;
    ok = ok && usb_serial_jtag_write_bytes(text, length, pdMS_TO_TICKS(1000)) == static_cast<int>(length);
    ok = ok && usb_serial_jtag_write_bytes("\r\n", 2, pdMS_TO_TICKS(300)) == 2;
    ok = ok && usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000)) == ESP_OK;
    funlockfile(stdout);
    return ok;
}

void Reply(uint32_t id, bool ok, const char* format = nullptr, ...) {
    static char line[512];
    int n = snprintf(line, sizeof(line), "@DEV {\"id\":%lu,\"ok\":%s", static_cast<unsigned long>(id),
                     ok ? "true" : "false");
    if (format) {
        va_list args;
        va_start(args, format);
        n += vsnprintf(line + n, sizeof(line) - n - 2, format, args);
        va_end(args);
    }
    if (n > static_cast<int>(sizeof(line)) - 2) n = sizeof(line) - 2;
    line[n++] = '}';
    WriteLine(line, n);
}

// ---- Injected input -------------------------------------------------------

struct Gesture {
    bool active;
    int x1, y1, x2, y2;
    int64_t start_us, duration_us;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Gesture s_gesture{};
int64_t s_key_release_us[3]{};   // BOOT, VOL+, VOL-; 0 = not held by us
esp_timer_handle_t s_timer = nullptr;
bool s_timer_running = false;

void Step(void*) {
    const int64_t now = esp_timer_get_time();
    bool busy = false;
    bool touch = false, release = false;
    int x = 0, y = 0;
    int key_up = -1;
    portENTER_CRITICAL(&s_mux);
    if (s_gesture.active) {
        const int64_t t = now - s_gesture.start_us;
        if (t >= s_gesture.duration_us) {
            s_gesture.active = false;
            release = true;
        } else {
            touch = busy = true;
            x = s_gesture.x1 + static_cast<int>((s_gesture.x2 - s_gesture.x1) * t / s_gesture.duration_us);
            y = s_gesture.y1 + static_cast<int>((s_gesture.y2 - s_gesture.y1) * t / s_gesture.duration_us);
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (!s_key_release_us[i]) continue;
        if (now >= s_key_release_us[i]) { s_key_release_us[i] = 0; key_up = i; }
        else busy = true;
    }
    if (!busy) s_timer_running = false;
    portEXIT_CRITICAL(&s_mux);
    if (touch) metalio_touch_dev_inject(true, x, y);
    if (release) metalio_touch_dev_inject(false, 0, 0);
    if (key_up >= 0) MetalioKeys_DevSetDown(key_up, false);
    if (!busy) esp_timer_stop(s_timer);
}

bool EnsureTimer() {
    if (!s_timer) {
        const esp_timer_create_args_t args = {.callback = Step, .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK, .name = "valet_dev", .skip_unhandled_events = true};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) return false;
    }
    bool start = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_timer_running) { s_timer_running = true; start = true; }
    portEXIT_CRITICAL(&s_mux);
    if (start) esp_timer_start_periodic(s_timer, kStepUs);
    return true;
}

// Raw touch coordinates equal portrait panel pixels; the cover keys report
// raw_y = TOUCH_VK_Y, below the panel.
const char* StartGesture(int x1, int y1, int x2, int y2, int ms) {
    if (ms < 30 || ms > 10000) return "BAD_DURATION";
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_gesture.active;
    if (!busy) s_gesture = {true, x1, y1, x2, y2, esp_timer_get_time(), static_cast<int64_t>(ms) * 1000};
    portEXIT_CRITICAL(&s_mux);
    if (busy) return "BUSY";
    metalio_touch_dev_inject(true, x1, y1);
    return EnsureTimer() ? nullptr : "TIMER";
}

const char* HoldKey(int index, int ms) {
    if (ms < 30 || ms > 60000) return "BAD_DURATION";
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_key_release_us[index] != 0;
    if (!busy) s_key_release_us[index] = esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
    portEXIT_CRITICAL(&s_mux);
    if (busy) return "BUSY";
    MetalioKeys_DevSetDown(index, true);
    return EnsureTimer() ? nullptr : "TIMER";
}

const char* PressKey(const char* name, int ms) {
    if (!strcmp(name, "home")) return StartGesture(TOUCH_VK_HOME_X, TOUCH_VK_Y, TOUCH_VK_HOME_X, TOUCH_VK_Y, ms);
    if (!strcmp(name, "prev")) return StartGesture(TOUCH_VK_PREV_X, TOUCH_VK_Y, TOUCH_VK_PREV_X, TOUCH_VK_Y, ms);
    if (!strcmp(name, "next")) return StartGesture(TOUCH_VK_NEXT_X, TOUCH_VK_Y, TOUCH_VK_NEXT_X, TOUCH_VK_Y, ms);
    if (!strcmp(name, "ai")) return HoldKey(0, ms);
    if (!strcmp(name, "vol_up")) return HoldKey(1, ms);
    if (!strcmp(name, "vol_down")) return HoldKey(2, ms);
    if (!strcmp(name, "power")) { MetalioKeys_DevPowerClick(); return nullptr; }
    return "UNKNOWN_KEY";
}

// ---- Screenshot -----------------------------------------------------------

// Sends the frame the UI last drew (native 4 bpp, landscape), deflated and
// base64-encoded. This is what the firmware asked the panel to show, not what
// the ink looks like.
void Screenshot(uint32_t id) {
    const int fb_w = epd_width(), fb_h = epd_height();
    const size_t raw = static_cast<size_t>(fb_w / 2) * fb_h;
    uLongf packed = compressBound(raw);
    auto* copy = static_cast<uint8_t*>(heap_caps_malloc(raw, MALLOC_CAP_SPIRAM));
    auto* out = static_cast<uint8_t*>(heap_caps_malloc(packed, MALLOC_CAP_SPIRAM));
    const char* error = nullptr;
    auto* display = LvAdapterEpdiy::Instance();
    const uint8_t* fb = EpdRenderBuffer();
    if (!copy || !out) error = "NO_MEMORY";
    else if (!display || !fb) error = "NO_DISPLAY";
    else if (!display->LockUi(3000)) error = "UI_BUSY";
    else {
        memcpy(copy, fb, raw);
        display->UnlockUi();
        if (compress2(out, &packed, copy, raw, 1) != Z_OK) error = "COMPRESS";
    }
    if (error) {
        Reply(id, false, ",\"code\":\"%s\"", error);
    } else {
        const auto stats = EpdGetStats();
        const uint32_t crc = crc32(0, copy, raw);
        const unsigned chunks = static_cast<unsigned>((packed + kChunk - 1) / kChunk);
        Reply(id, true, ",\"w\":%d,\"h\":%d,\"bpp\":4,\"raw\":%u,\"z\":%lu,\"crc\":%lu,\"chunks\":%u,"
                        "\"orient\":%d,\"epd_busy\":%s",
              fb_w, fb_h, static_cast<unsigned>(raw), static_cast<unsigned long>(packed),
              static_cast<unsigned long>(crc), chunks, DisplayUiGetOrient(), stats.busy ? "true" : "false");
        static char line[800];
        for (unsigned i = 0; i < chunks; ++i) {
            const size_t offset = static_cast<size_t>(i) * kChunk;
            const size_t take = packed - offset < kChunk ? packed - offset : kChunk;
            int n = snprintf(line, sizeof(line), "@DEVD %lu %u ", static_cast<unsigned long>(id), i);
            size_t written = 0;
            if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(line + n), sizeof(line) - n, &written,
                                      out + offset, take) != 0 ||
                !WriteLine(line, n + written)) {
                ESP_LOGW(TAG, "screenshot aborted at chunk %u/%u", i, chunks);
                break;
            }
        }
    }
    heap_caps_free(copy);
    heap_caps_free(out);
}

void Info(uint32_t id) {
    const auto stats = EpdGetStats();
    char screen[40] = "?";
    if (auto* display = LvAdapterEpdiy::Instance(); display && display->LockUi(500)) {
        snprintf(screen, sizeof(screen), "%s", VkKey_ActiveScreen());
        display->UnlockUi();
    }
    const auto* app = esp_app_get_description();
    Reply(id, true, ",\"screen\":\"%s\",\"standby\":%s,\"orient\":%d,\"ui_w\":%d,\"ui_h\":%d,"
                    "\"panel_w\":%d,\"panel_h\":%d,\"key_y\":%d,\"key_x\":[%d,%d,%d],"
                    "\"epd_busy\":%s,\"epd_submitted\":%lu,\"epd_completed\":%lu,"
                    "\"uptime_ms\":%lld,\"heap\":%u,\"version\":\"%s\"",
          screen, StandbyScreen::IsActive() ? "true" : "false", DisplayUiGetOrient(), DisplayUiLogW(), DisplayUiLogH(),
          kPanelW, kPanelH, TOUCH_VK_Y, TOUCH_VK_HOME_X, TOUCH_VK_PREV_X, TOUCH_VK_NEXT_X,
          stats.busy ? "true" : "false", static_cast<unsigned long>(stats.submitted),
          static_cast<unsigned long>(stats.completed), static_cast<long long>(esp_timer_get_time() / 1000),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), app ? app->version : "");
}

bool Int(const cJSON* root, const char* name, int* out, int fallback, bool required = false) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item)) { *out = fallback; return !required; }
    *out = item->valueint;
    return true;
}

} // namespace

void ValetDevLinkHandle(const char* json) {
    cJSON* root = cJSON_Parse(json);
    const auto* id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    const auto* cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(id_item) || !cJSON_IsString(cmd_item)) {
        cJSON_Delete(root);
        Reply(0, false, ",\"code\":\"INVALID_REQUEST\"");
        return;
    }
    const auto id = static_cast<uint32_t>(id_item->valuedouble);
    const char* cmd = cmd_item->valuestring;
    const char* error = nullptr;
    bool replied = false;
    int x1, y1, x2, y2, ms;
    if (!strcmp(cmd, "info")) { Info(id); replied = true; }
    else if (!strcmp(cmd, "shot")) { Screenshot(id); replied = true; }
    else if (!strcmp(cmd, "tap") || !strcmp(cmd, "swipe")) {
        const bool swipe = cmd[0] == 's';
        if (!Int(root, "x", &x1, 0, true) || !Int(root, "y", &y1, 0, true) ||
            !Int(root, "x2", &x2, x1, swipe) || !Int(root, "y2", &y2, y1, swipe)) error = "MISSING_POINT";
        else if (x1 < 0 || x1 >= kPanelW || y1 < 0 || y1 >= kPanelH ||
                 x2 < 0 || x2 >= kPanelW || y2 < 0 || y2 >= kPanelH) error = "OUT_OF_PANEL";
        else { Int(root, "ms", &ms, swipe ? 400 : 100); error = StartGesture(x1, y1, x2, y2, ms); }
    } else if (!strcmp(cmd, "key")) {
        const auto* name = cJSON_GetObjectItemCaseSensitive(root, "name");
        Int(root, "ms", &ms, 100);
        error = cJSON_IsString(name) ? PressKey(name->valuestring, ms) : "MISSING_NAME";
    } else if (!strcmp(cmd, "home")) {
        ScreenRequestHome();
    } else error = "UNKNOWN_COMMAND";
    cJSON_Delete(root);
    if (replied) return;
    if (error) Reply(id, false, ",\"code\":\"%s\"", error);
    else Reply(id, true);
}

#else

void ValetDevLinkHandle(const char*) {}

#endif
