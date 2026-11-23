#include "settings_network_signal.h"
#include "assets/lang_cellular.h"
#include "assets/lang_config.h"
#include "board.h"
#include "dual_network_board.h"
#include "nt26_board.h"
#include "wifi_board.h"
#include "wifi_station.h"
#include "fontpack_lvgl.h"
#include "screen_common.h"
#include "settings_common.h"
#include "ui_scale.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "SettingsSignal";
constexpr uint32_t kSignalPollMs = 2000; // 采样周期；4G 查询需占用 AT 通道
constexpr lv_coord_t kSignalTitleW = UiSx(120);
constexpr size_t kSignalTextMax = 48;
constexpr size_t kIccidTextMax = 32;

// 采样在后台执行；页面删除时递增代数，使迟到结果失效。
// What the status card shows. Sampled off the UI task: reading the 4G signal talks to the modem.
enum class LinkState : uint8_t {
    kOnline,      // connected, strength in `value` (4G: CSQ 0–31, Wi-Fi: dBm)
    kOnlineBlind, // connected, strength not read (yet)
    kOffline,     // 4G: not registered
    kWifiOff,     // Wi-Fi switched off by the user
    kParked,      // Wi-Fi radio parked to save power; it comes back when something goes online
    kConnecting,  // Wi-Fi on but not associated
};
struct CardSample {
    bool cell = false;
    LinkState state = LinkState::kOnlineBlind;
    int value = 0;
    char name[33] = {}; // Wi-Fi network name
};

struct SignalUiMsg {
    uint32_t generation = 0; // 信号栏会话代数
    char text[kSignalTextMax] = {}; // 格式化后的信号状态
    CardSample card;
};

lv_obj_t* s_signal_value = nullptr;
lv_obj_t* s_iccid_value = nullptr;
lv_timer_t* s_signal_timer = nullptr;
std::atomic<bool> s_signal_alive{false};
std::atomic<uint32_t> s_signal_generation{0};
std::atomic<bool> s_signal_busy{false};
char s_last_signal_text[kSignalTextMax] = {};
char s_last_iccid_text[kIccidTextMax] = {};
// Valet status card
std::atomic<bool> s_card_mode{false}; // this session draws the card, not the rows
lv_obj_t* s_card_holder = nullptr;    // keeps the card's place while the card is rebuilt
lv_obj_t* s_card = nullptr;
bool s_card_ok = true;                // drawn solid (fine) or dashed (needs attention)
CardSample s_card_shown;              // what the card says now
bool s_iccid_with_label = false;      // the number is one line: label and number together

const char* ValetText(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}

bool IsCellularMode() {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    return dual != nullptr && dual->GetNetworkType() == NetworkType::CELLULAR;
}

Nt26Board* CurrentNt26() {
    auto& board = Board::GetInstance();
    auto* dual = dynamic_cast<DualNetworkBoard*>(&board);
    return dynamic_cast<Nt26Board*>(dual != nullptr ? &dual->GetCurrentBoard() : &board);
}

void StopSignalTimer() {
    if (s_signal_timer != nullptr) {
        lv_timer_delete(s_signal_timer);
        s_signal_timer = nullptr;
    }
}

void InvalidateSignalSession() {
    s_signal_alive.store(false, std::memory_order_release);
    s_signal_generation.fetch_add(1, std::memory_order_acq_rel);
    StopSignalTimer();
    s_signal_value = nullptr;
    s_iccid_value = nullptr;
    s_last_signal_text[0] = '\0';
    s_last_iccid_text[0] = '\0';
    s_card_mode.store(false, std::memory_order_release);
    s_card_holder = nullptr;
    s_card = nullptr;
    s_iccid_with_label = false;
}

void FormatWifiSignal(char* ui, size_t ui_len) {
    auto& wifi = WifiStation::GetInstance();
    if (!wifi.IsConnected()) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_DISCONNECTED);
        return;
    }

    // 读取失败时明确显示状态，避免将无效 RSSI 当作有效信号。
    wifi_ap_record_t ap = {};
    if (!wifi.TryGetApInfo(ap)) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_READ_FAIL);
        return;
    }

    const int rssi = static_cast<int>(ap.rssi);
    std::snprintf(ui, ui_len, Lang::Strings::SETTINGS_TEST_SIGNAL_WIFI_RSSI_FMT, rssi);
}

void FormatCellSignal(char* ui, size_t ui_len) {
    auto* nt26 = CurrentNt26();
    if (nt26 == nullptr) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_UNAVAIL);
        return;
    }

    // 可能阻塞 AT；仅允许在采样任务线程调用。
    const int csq = nt26->GetSignalStrength();
    if (csq == 99 || csq < 0) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_CSQ_UNKNOWN);
        return;
    }

    std::snprintf(ui, ui_len, Lang::Strings::SETTINGS_TEST_SIGNAL_CELL_CSQ_FMT, csq);
}

void SampleSignalText(char* ui, size_t ui_len) {
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual == nullptr && dynamic_cast<WifiBoard*>(&Board::GetInstance()) == nullptr) {
        std::snprintf(ui, ui_len, "%s", Lang::Strings::SETTINGS_TEST_SIGNAL_BOARD_UNSUP);
        return;
    }

    if (dual == nullptr || dual->GetNetworkType() == NetworkType::WIFI) {
        FormatWifiSignal(ui, ui_len);
    } else {
        FormatCellSignal(ui, ui_len);
    }
}

void RefreshIccidFromCache() {
    if (s_iccid_value == nullptr || !lv_obj_is_valid(s_iccid_value)) return;
    auto* nt26 = CurrentNt26();
    const std::string iccid = nt26 != nullptr ? nt26->GetIccid() : std::string{};
    const char* text = iccid.empty() ? Lang::Strings::SETTINGS_TEST_SIGNAL_ICCID_UNKNOWN : iccid.c_str();
    if (std::strncmp(s_last_iccid_text, text, kIccidTextMax) == 0) return;
    if (s_iccid_with_label) lv_label_set_text_fmt(s_iccid_value, "%s %s", ValetText("卡号（ICCID）", "Card number (ICCID)"), text);
    else lv_label_set_text(s_iccid_value, text);
    std::strncpy(s_last_iccid_text, text, kIccidTextMax - 1);
    s_last_iccid_text[kIccidTextMax - 1] = '\0';
}

// ── Valet status card ──────────────────────────────────────────────────────
// `with_modem` false leaves out the one call that waits on the 4G modem, so it can run on the UI task.
CardSample SampleCard(bool with_modem) {
    CardSample sample;
    sample.cell = IsCellularMode();
    if (sample.cell) {
        auto* nt26 = CurrentNt26();
        if (nt26 != nullptr && !nt26->IsNetworkReady() && nt26->GetNetworkBusyStatusText() != nullptr) {
            // Registering (just switched over, or woken up): a matter of seconds, not a fault.
            sample.state = LinkState::kConnecting;
        } else if (nt26 == nullptr || !nt26->IsNetworkReady()) {
            sample.state = LinkState::kOffline;
        } else if (with_modem) {
            const int csq = nt26->GetSignalStrength();
            if (csq >= 0 && csq <= 31) {
                sample.state = LinkState::kOnline;
                sample.value = csq;
            }
        }
        return sample;
    }
    auto& wifi = WifiStation::GetInstance();
    if (!wifi.IsUserEnabled()) sample.state = LinkState::kWifiOff;
    else if (wifi.IsLpPaused()) sample.state = LinkState::kParked;
    else if (!wifi.IsConnected()) sample.state = LinkState::kConnecting;
    else {
        wifi_ap_record_t ap = {};
        if (wifi.TryGetApInfo(ap)) {
            sample.state = LinkState::kOnline;
            sample.value = static_cast<int>(ap.rssi);
            std::snprintf(sample.name, sizeof(sample.name), "%s", reinterpret_cast<const char*>(ap.ssid));
        }
    }
    return sample;
}

// Strength in words. 4G uses the status bar's steps; Wi-Fi the steps of its icon.
const char* StrengthWord(const CardSample& sample) {
    const bool good = sample.cell ? sample.value >= 15 : sample.value >= -60;
    const bool fair = sample.cell ? sample.value >= 10 : sample.value >= -70;
    return good ? ValetText("信号好", "good signal") : fair ? ValetText("信号一般", "fair signal") : ValetText("信号弱", "weak signal");
}

bool CardIsFine(const CardSample& sample) {
    return sample.state == LinkState::kOnline || sample.state == LinkState::kOnlineBlind || sample.state == LinkState::kParked ||
           (sample.cell && sample.state == LinkState::kConnecting);
}

void CardTexts(const CardSample& sample, char* title, size_t title_len, char* detail, size_t detail_len) {
    const char* link = sample.cell ? "4G" : "Wi-Fi";
    switch (sample.state) {
        case LinkState::kOnline:
            std::snprintf(title, title_len, "%s · %s", link, StrengthWord(sample));
            if (sample.cell) std::snprintf(detail, detail_len, ValetText("已连上 · 强度 %d / 31", "Connected · strength %d / 31"), sample.value);
            else std::snprintf(detail, detail_len, "%s · %d dBm", sample.name, sample.value);
            break;
        case LinkState::kOnlineBlind:
            std::snprintf(title, title_len, "%s · %s", link, ValetText("已连上", "connected"));
            std::snprintf(detail, detail_len, "%s", ValetText("信号强度还没读到", "Signal strength not read yet"));
            break;
        case LinkState::kOffline:
            std::snprintf(title, title_len, "%s · %s", link, ValetText("没连上", "not connected"));
            std::snprintf(detail, detail_len, "%s", ValetText("正在找网络。一直不行就看看卡和信号", "Looking for the network. Check the SIM and signal"));
            break;
        case LinkState::kWifiOff:
            std::snprintf(title, title_len, "%s · %s", link, ValetText("关着", "switched off"));
            std::snprintf(detail, detail_len, "%s", ValetText("打开 Wi-Fi 后才能连网", "Turn Wi-Fi on to go online"));
            break;
        case LinkState::kParked:
            std::snprintf(title, title_len, "%s · %s", link, ValetText("省电待命", "parked to save power"));
            std::snprintf(detail, detail_len, "%s", ValetText("要用网时自动连上", "Reconnects when something goes online"));
            break;
        case LinkState::kConnecting:
            if (sample.cell) {
                std::snprintf(title, title_len, "%s · %s", link, ValetText("正在连", "connecting"));
                std::snprintf(detail, detail_len, "%s", ValetText("正在注册网络，一般十秒左右", "Registering with the network, usually about ten seconds"));
                break;
            }
            std::snprintf(title, title_len, "%s · %s", link, ValetText("没连上", "not connected"));
            std::snprintf(detail, detail_len, "%s", ValetText("正在连接。连不上就点下面的 Wi-Fi 换网络", "Connecting. If it fails, pick a network below"));
            break;
    }
}

// The reading moves a little all the time, and every change of text is a panel refresh.
// Keep the number on the card until it has moved enough to matter or the wording changes.
bool WorthRedrawing(const CardSample& shown, const CardSample& now) {
    if (shown.cell != now.cell || shown.state != now.state) return true;
    if (now.state != LinkState::kOnline) return false;
    if (std::strcmp(shown.name, now.name) != 0 || StrengthWord(shown) != StrengthWord(now)) return true;
    const int moved = now.value > shown.value ? now.value - shown.value : shown.value - now.value;
    return moved >= (now.cell ? 3 : 5);
}

void ShowCard(const CardSample& sample, bool first) {
    if (s_card_holder == nullptr || !lv_obj_is_valid(s_card_holder)) return;
    if (!first && !WorthRedrawing(s_card_shown, sample)) return;
    char title[64], detail[96];
    CardTexts(sample, title, sizeof(title), detail, sizeof(detail));
    const bool fine = CardIsFine(sample);
    if (first || fine != s_card_ok) {
        // Solid and dashed are different cards: build the other one in the same place.
        lv_obj_clean(s_card_holder);
        s_card = SettingsValetCard(s_card_holder, title, detail, fine);
        s_card_ok = fine;
    } else {
        SettingsValetCardSet(s_card, title, detail);
    }
    s_card_shown = sample;
}

void ApplySignalUi(void* p) {
    auto* msg = static_cast<SignalUiMsg*>(p);
    if (msg == nullptr) {
        return;
    }

    const bool current = s_signal_alive.load(std::memory_order_acquire) &&
                         msg->generation == s_signal_generation.load(std::memory_order_acquire);
    if (current && s_card_mode.load(std::memory_order_acquire)) {
        ShowCard(msg->card, false);
        RefreshIccidFromCache();
        delete msg;
        return;
    }
    const bool live = current && s_signal_value != nullptr && lv_obj_is_valid(s_signal_value);
    if (live) {
        if (std::strncmp(s_last_signal_text, msg->text, kSignalTextMax) != 0) {
            lv_label_set_text(s_signal_value, msg->text);
            std::strncpy(s_last_signal_text, msg->text, kSignalTextMax - 1);
            s_last_signal_text[kSignalTextMax - 1] = '\0';
        }
        RefreshIccidFromCache();
    }
    delete msg;
}

void SignalSampleTask(void* /*arg*/) {
    const uint32_t gen = s_signal_generation.load(std::memory_order_acquire);

    char ui[kSignalTextMax] = {};
    CardSample card;
    if (s_card_mode.load(std::memory_order_acquire)) card = SampleCard(true);
    else SampleSignalText(ui, sizeof(ui));

    if (!s_signal_alive.load(std::memory_order_acquire) ||
        gen != s_signal_generation.load(std::memory_order_acquire)) {
        s_signal_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    auto* msg = new (std::nothrow) SignalUiMsg{};
    if (msg == nullptr) {
        ESP_LOGW(TAG, "signal ui msg alloc failed");
        s_signal_busy.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    msg->generation = gen;
    msg->card = card;
    std::strncpy(msg->text, ui, sizeof(msg->text) - 1);
    msg->text[sizeof(msg->text) - 1] = '\0';

    // busy 在投递成功后仍由本任务清掉：下一拍可再采样；迟到 Apply 靠 generation 丢弃。
    if (!ScreenLvAsyncUrgent(ApplySignalUi, msg)) {
        ESP_LOGW(TAG, "PostLvAsync signal ui failed");
        delete msg;
    }
    s_signal_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void KickSignalSample() {
    if (!s_signal_alive.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!s_signal_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const BaseType_t ok =
        xTaskCreate(SignalSampleTask, "stest_sig", 4096, nullptr, 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "signal sample task create failed");
        s_signal_busy.store(false, std::memory_order_release);
    }
}

void OnSignalPollTimer(lv_timer_t* /*t*/) {
    KickSignalSample();
    RefreshIccidFromCache();
}

lv_obj_t* CreateInfoRow(lv_obj_t* page, const char* title_text, const char* value_text) {
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, UiSy(4), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, kSignalTitleW);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, value_text);
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(value, lv_color_black(), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_clear_flag(value, LV_OBJ_FLAG_CLICKABLE);
    return value;
}

/** @brief ICCID 标题与号码上下分行，避免长号码横向挤换行 */
lv_obj_t* CreateIccidRow(lv_obj_t* page) {
    lv_obj_t* row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, UiSy(4), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(row, UiSy(2), 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, Lang::Strings::SETTINGS_TEST_SIGNAL_ICCID_TITLE);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, Lang::Strings::SETTINGS_TEST_SIGNAL_ICCID_UNKNOWN);
    lv_obj_set_width(value, lv_pct(100));
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(value, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(value, lv_color_black(), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(value, LV_OBJ_FLAG_CLICKABLE);
    return value;
}

}  // namespace

void SettingsNetworkSignal_Build(lv_obj_t* parent) {
    InvalidateSignalSession();
    s_signal_value = CreateInfoRow(parent, Lang::Strings::SETTINGS_TEST_SIGNAL_TITLE,
                                   Lang::Strings::SETTINGS_TEST_SIGNAL_READING);
    if (IsCellularMode()) {
        s_iccid_value = CreateIccidRow(parent);
        RefreshIccidFromCache();
    }
    lv_obj_add_event_cb(s_signal_value, [](lv_event_t* e) {
        if (lv_event_get_target(e) == s_signal_value) {
            InvalidateSignalSession();
        }
    }, LV_EVENT_DELETE, nullptr);
    s_signal_alive.store(true, std::memory_order_release);
    s_signal_timer = lv_timer_create(OnSignalPollTimer, kSignalPollMs, nullptr);
    KickSignalSample();
}

void SettingsNetworkSignal_BuildCard(lv_obj_t* parent) {
    InvalidateSignalSession();
    s_card_holder = lv_obj_create(parent);
    lv_obj_remove_style_all(s_card_holder);
    lv_obj_set_size(s_card_holder, lv_pct(100), 138);
    lv_obj_clear_flag(s_card_holder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_card_holder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card_holder, [](lv_event_t* e) {
        if (lv_event_get_target(e) == s_card_holder) InvalidateSignalSession();
    }, LV_EVENT_DELETE, nullptr);
    // What can be known without asking the modem goes on the card at once, so the page
    // does not open on a placeholder; the signal reading follows from the sampling task.
    ShowCard(SampleCard(false), true);
    s_card_mode.store(true, std::memory_order_release);
    s_signal_alive.store(true, std::memory_order_release);
    s_signal_timer = lv_timer_create(OnSignalPollTimer, kSignalPollMs, nullptr);
    KickSignalSample();
}

void SettingsNetworkSignal_BuildCardNumber(lv_obj_t* parent) {
    if (!IsCellularMode() || s_card_holder == nullptr) return;
    s_iccid_value = SettingsValetNote(parent, "");
    s_iccid_with_label = true;
    s_last_iccid_text[0] = '\0';
    RefreshIccidFromCache();
}
