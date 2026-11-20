#include "settings_log_tab.h"

#include "application.h"
#include "assets/lang_config.h"
#include "assets/lang_log.h"
#include "board.h"
#include "display.h"
#include "fontpack_lvgl.h"
#include "sd_log_writer.h"
#include "settings_common.h"
#include "settings_log_upload.h"

#include "sdkconfig.h"

#include <atomic>
#include <cstring>

struct SettingsLogOption {
    bool enabled; // 是否写 SD 日志
    const char* (*title)(); // 选项标题
    const char* (*current_text)(); // 当前选中文案
};

static const char* LogTitleOn() { return Lang::Strings::SETTINGS_LOG_ON; }
static const char* LogTitleOff() { return Lang::Strings::SETTINGS_LOG_OFF; }
static const char* LogCurOn() { return Lang::Strings::SETTINGS_LOG_CURRENT_ON; }
static const char* LogCurOff() { return Lang::Strings::SETTINGS_LOG_CURRENT_OFF; }

static constexpr SettingsLogOption kOptions[] = {
    {true, LogTitleOn, LogCurOn},
    {false, LogTitleOff, LogCurOff},
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

struct SettingsLogUi {
    lv_obj_t* btns[kOptionCount] = {}; // 选项按钮
    lv_obj_t* current_lbl = nullptr; // 当前状态文案
    lv_obj_t* upload_btn = nullptr; // 上传按钮
    lv_obj_t* clear_btn = nullptr;
    lv_obj_t* clear_status = nullptr;
};

static SettingsLogUi s_ui;
static std::atomic<bool> s_clearing{false}; // UI 与后台任务共享的清理状态

static const char* CurrentText(bool enabled) {
    for (int i = 0; i < kOptionCount; ++i) {
        if (kOptions[i].enabled == enabled) {
            return kOptions[i].current_text();
        }
    }
    return kOptions[0].current_text();
}

static lv_obj_t* s_valet_seg = nullptr;   // Valet layout: record on / off
static void RefreshOptions() {
    const bool enabled = SdLogWriter_IsEnabled();
    if (s_valet_seg) {
        SettingsValetSelect(s_valet_seg, enabled ? 0 : 1);
        // The two actions keep their look; while a clear runs they only stop taking taps.
        for (auto* button : {s_ui.upload_btn, s_ui.clear_btn}) {
            if (!button) continue;
            if (s_clearing.load()) lv_obj_add_state(button, LV_STATE_DISABLED);
            else lv_obj_remove_state(button, LV_STATE_DISABLED);
        }
        return;
    }
    auto refresh_button = [](lv_obj_t* button, bool selected) {
        if (button == nullptr) return;
        if (s_clearing.load()) lv_obj_add_state(button, LV_STATE_DISABLED);
        else lv_obj_remove_state(button, LV_STATE_DISABLED);
        SettingsStyleSelectable(button, lv_obj_get_child(button, 0), selected);
    };
    for (int i = 0; i < kOptionCount; ++i) {
        refresh_button(s_ui.btns[i], kOptions[i].enabled == enabled);
    }
    if (s_ui.current_lbl != nullptr) {
        lv_label_set_text(s_ui.current_lbl, CurrentText(enabled));
    }
    refresh_button(s_ui.upload_btn, false);
    refresh_button(s_ui.clear_btn, false);
}

static void OnOptionClicked(lv_event_t* e) {
    if (s_clearing.load() || SettingsLogUpload_IsActive()) {
        return;
    }
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    if (SdLogWriter_IsEnabled() == enable) {
        return;
    }
    SdLogWriter_SetEnabled(enable);
    RefreshOptions();
}

static void OnUploadClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_clearing.load()) {
        return;
    }
    SettingsLogUpload_Start();
}

static void OnClearClicked(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_clearing.load() || SettingsLogUpload_IsActive()) {
        return;
    }
    s_clearing.store(true);
    lv_label_set_text(s_ui.clear_status, Lang::Strings::SETTINGS_LOG_CLEARING);
    RefreshOptions();
    // The LVGL task uses a PSRAM stack. Perform SD I/O on the application's
    // internal-RAM task and acquire the display lock only for the result.
    if (!Application::GetInstance().Schedule([]() {
        const bool ok = SdLogWriter_Clear();
        auto* display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        s_clearing.store(false);
        if (!lock) return;
        RefreshOptions();
        // Reset clears the pointers on page deletion; a rebuilt log tab should
        // receive this result too rather than remaining stuck on "Clearing".
        if (s_ui.clear_status != nullptr) {
            lv_label_set_text(s_ui.clear_status, ok ? Lang::Strings::SETTINGS_LOG_CLEAR_OK
                                                   : Lang::Strings::SETTINGS_LOG_CLEAR_FAIL);
        }
    }, Application::TaskDomain::Storage)) {
        s_clearing.store(false);
        RefreshOptions();
        lv_label_set_text(s_ui.clear_status, Lang::Strings::SETTINGS_LOG_CLEAR_FAIL);
    }
}

void SettingsLogTab_Reset() {
    SettingsLogUpload_Teardown();
    s_ui = {};
    s_valet_seg = nullptr;
}

void SettingsLogTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_valet_seg = nullptr;
    #if !CONFIG_BOARD_TYPE_METALIO_E_INK4_PLUS_GLOBAL
    {
        const bool zh = std::strcmp(Lang::Current(), "zh-CN") == 0;
        lv_obj_set_style_pad_row(page, 0, 0);
        SettingsValetLabel(page, zh ? "运行日志" : "Runtime log");
        const char* const names[] = {zh ? "记录" : "Record", zh ? "不记录" : "Off"};
        static const intptr_t kValues[] = {1, 0};
        s_valet_seg = SettingsValetSegments(page, names, 2, OnOptionClicked, kValues);
        SettingsValetNote(page, zh ? "记在 SD 卡的 metalio/e-ink/logs 里。\n出了问题时上传，方便查原因。"
                                   : "Kept on the SD card in metalio/e-ink/logs. Upload it when something goes wrong.");
        SettingsValetGap(page, 20);
        auto* actions = SettingsValetPairs(page);
        s_ui.upload_btn = SettingsValetButton(actions, Lang::Strings::SETTINGS_LOG_UPLOAD, OnUploadClicked);
        s_ui.clear_btn = SettingsValetButton(actions, Lang::Strings::SETTINGS_LOG_CLEAR, OnClearClicked);
        SettingsValetHalf(s_ui.upload_btn);
        SettingsValetHalf(s_ui.clear_btn);
        s_ui.clear_status = SettingsValetNote(page, s_clearing.load() ? Lang::Strings::SETTINGS_LOG_CLEARING : "");
        RefreshOptions();
        return;
    }
    #endif

    lv_obj_t* title = lv_label_create(page);
    lv_label_set_text(title, Lang::Strings::SETTINGS_LOG_TITLE);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint = lv_label_create(page);
    lv_label_set_text(hint, Lang::Strings::SETTINGS_LOG_HINT);
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* hint_off = lv_label_create(page);
    lv_label_set_text(hint_off, Lang::Strings::SETTINGS_LOG_HINT_OFF);
    lv_obj_set_width(hint_off, lv_pct(100));
    lv_label_set_long_mode(hint_off, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint_off, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(hint_off, lv_color_black(), 0);
    lv_obj_clear_flag(hint_off, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kOptionCount; ++i) {
        s_ui.btns[i] = SettingsCreateSelectableOption(
            page, kOptions[i].title(), OnOptionClicked,
            static_cast<intptr_t>(kOptions[i].enabled ? 1 : 0));
    }

    s_ui.current_lbl = lv_label_create(page);
    lv_label_set_text(s_ui.current_lbl, "");
    lv_obj_set_style_text_font(s_ui.current_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.current_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.current_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.upload_btn = SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_LOG_UPLOAD,
                                                     OnUploadClicked, 0);

    s_ui.clear_btn = SettingsCreateSelectableOption(page, Lang::Strings::SETTINGS_LOG_CLEAR,
                                                    OnClearClicked, 0);
    s_ui.clear_status = lv_label_create(page);
    lv_obj_set_width(s_ui.clear_status, lv_pct(100));
    lv_label_set_text(s_ui.clear_status, s_clearing.load() ? Lang::Strings::SETTINGS_LOG_CLEARING : "");
    lv_obj_set_style_text_font(s_ui.clear_status, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_ui.clear_status, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.clear_status, LV_OBJ_FLAG_CLICKABLE);

    RefreshOptions();
}
