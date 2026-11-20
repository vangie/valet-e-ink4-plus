#include "standby_screen/standby_classic_priv.h"

#include "standby_classic_priv.h"
#include "standby_classic_ui.h"
#include "standby_classic_weather_store.h"
#include "standby_classic_weather_fetch.h"
#include "standby_classic_todos.h"
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_log.h>
#include <lvgl.h>

#include "assets/lang_config.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "home_screen/home_valet_font.h"
#include "haptic_feedback.h"
#include "home_icon_source.h"

// Valet standby page. The panel is drawn once when the device goes to sleep and
// is not redrawn until it wakes, so the page shows what stays true for the whole
// sleep: the date (large, where the design has a clock), the weekday and lunar
// date, the weather with the battery level, and the open to-dos.
namespace {
constexpr int kValetSide = 33;
lv_obj_t* s_valet_weekday = nullptr;
lv_obj_t* s_valet_battery = nullptr;
const lv_font_t* ValetSmall() { const auto* f = fontpack_lv_font_get(25, 2); return f ? f : fontpack_lv_font_ui(); }
const lv_font_t* ValetStrong() { const auto* f = fontpack_lv_font_get(30, 4); return f ? f : fontpack_lv_font_ui(); }
lv_obj_t* ValetLabel(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}
lv_obj_t* ValetRule(lv_obj_t* parent, int y) {
    auto* rule = lv_obj_create(parent);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_HOR_RES - 2 * kValetSide, 3);
    lv_obj_set_pos(rule, kValetSide, y);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    return rule;
}
// The slanted stroke between month and day, as on the home page's clock.
void DrawDateSlash(lv_event_t* e) {
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = 3;
    line.opa = LV_OPA_COVER;
    line.round_start = line.round_end = 1;
    line.p1 = {static_cast<lv_value_precise_t>(a.x1), static_cast<lv_value_precise_t>(a.y2 - 2)};
    line.p2 = {static_cast<lv_value_precise_t>(a.x2), static_cast<lv_value_precise_t>(a.y1 + 2)};
    lv_draw_line(lv_event_get_layer(e), &line);
}
}

void StandbyClassic_RefreshDateOnce() {
    auto& ui = StandbyClassic_State();
    if (ui.date_lbl == nullptr) {
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr || tm_info.tm_year < (2020 - 1900)) {
        lv_label_set_text(ui.date_lbl, "--");
        lv_label_set_text(ui.lunar_lbl, "--");
        if (s_valet_weekday) lv_label_set_text(s_valet_weekday, "");
        return;
    }
    char text[8];
    std::snprintf(text, sizeof(text), "%02d", tm_info.tm_mon + 1);
    lv_label_set_text(ui.date_lbl, text);
    std::snprintf(text, sizeof(text), "%02d", tm_info.tm_mday);
    lv_label_set_text(ui.lunar_lbl, text);
    static const char* const kZh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    static const char* const kEn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    if (s_valet_weekday) {
        const bool zh = std::strcmp(Lang::Current(), "zh-CN") == 0;
        // The title font holds the Chinese weekday characters only; English uses the UI font.
        lv_obj_set_style_text_font(s_valet_weekday, zh ? &kValetTitleFont : ValetStrong(), 0);
        lv_label_set_text(s_valet_weekday, (zh ? kZh : kEn)[tm_info.tm_wday]);
    }
    if (s_valet_battery) {
        char battery[32] = "";
        int level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(level, charging, discharging))
            std::snprintf(battery, sizeof(battery), "%s %d%%", std::strcmp(Lang::Current(), "zh-CN") == 0 ? "电量" : "Battery", level);
        lv_label_set_text(s_valet_battery, battery);
    }
}

void StandbyClassic_ApplyWeatherUi() {
    auto& ui = StandbyClassic_State();
    if (!StandbyClassic_alive || ui.weather_temp == nullptr) {
        return;
    }
    const WeatherSnap snap = StandbyClassic_WeatherRamCopy();
    char lunar[24] = "";
    if (snap.valid && std::strcmp(Lang::Current(), "zh-CN") == 0) StandbyClassic_FormatLunarOrWeekday(lunar, sizeof(lunar), snap.lunar);
    // The lunar font holds only the characters lunar dates are made of.
    bool covered = true;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(lunar); *p && covered;) {
        uint32_t cp = *p;
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        for (++p; extra > 0 && *p; --extra, ++p) cp = (cp << 6) | (*p & 0x3F);
        lv_font_glyph_dsc_t glyph{};
        covered = lv_font_get_glyph_dsc(&kValetLunarFont, &glyph, cp, 0) && !glyph.is_placeholder;
    }
    lv_obj_set_style_text_font(ui.weather_text, covered ? &kValetLunarFont : ValetStrong(), 0);
    lv_label_set_text(ui.weather_text, lunar);
    char weather[64] = "";
    if (snap.valid && snap.has_temp) std::snprintf(weather, sizeof(weather), "%s %d°", StandbyClassic_LocalizedPhenomText(snap), snap.temp);
    else if (snap.valid) std::snprintf(weather, sizeof(weather), "%s", StandbyClassic_LocalizedPhenomText(snap));
    lv_label_set_text(ui.weather_temp, weather);
}

void StandbyClassic_BuildUi(lv_obj_t* root) {
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    StandbyClassic_todos.clear();
    auto& ui = StandbyClassic_State();
    const int width = LV_HOR_RES;

    // Month over day, the day shifted right, a rising stroke between them.
    ui.date_lbl = ValetLabel(root, "--", &kValetBigDateFont);
    lv_obj_set_pos(ui.date_lbl, kValetSide + 14, 108);
    ui.lunar_lbl = ValetLabel(root, "--", &kValetBigDateFont);   // the day of the month
    lv_obj_set_pos(ui.lunar_lbl, kValetSide + 30, 336);
    auto* slash = lv_obj_create(root);
    lv_obj_remove_style_all(slash);
    lv_obj_set_size(slash, 250, 52);
    lv_obj_set_pos(slash, kValetSide + 16, 300);
    lv_obj_clear_flag(slash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slash, DrawDateSlash, LV_EVENT_DRAW_MAIN, nullptr);

    // Right column: lunar date over the weekday, sitting on the day's baseline.
    auto* column = lv_obj_create(root);
    lv_obj_remove_style_all(column);
    lv_obj_set_size(column, 320, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(column, 10, 0);
    lv_obj_clear_flag(column, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    ui.weather_text = ValetLabel(column, "", &kValetLunarFont);    // lunar date
    s_valet_weekday = ValetLabel(column, "", &kValetTitleFont);
    lv_obj_update_layout(column);
    lv_obj_align(column, LV_ALIGN_TOP_RIGHT, -kValetSide, 562 - 116);

    // Weather on the left, battery on the right, between two rules.
    ValetRule(root, 644);
    ui.weather_temp = ValetLabel(root, "", ValetStrong());
    lv_obj_set_pos(ui.weather_temp, kValetSide, 678);
    s_valet_battery = ValetLabel(root, "", fontpack_lv_font_ui());
    lv_obj_align(s_valet_battery, LV_ALIGN_TOP_RIGHT, -kValetSide, 678);
    ValetRule(root, 750);

    auto* caption = ValetLabel(root, std::strcmp(Lang::Current(), "zh-CN") == 0 ? "今日待办" : "To-do today", ValetSmall());
    lv_obj_set_pos(caption, kValetSide, 784);
    ui.todo_host = lv_obj_create(root);
    lv_obj_remove_style_all(ui.todo_host);
    lv_obj_set_size(ui.todo_host, width - 2 * kValetSide, LV_SIZE_CONTENT);
    lv_obj_set_pos(ui.todo_host, kValetSide, 830);
    lv_obj_set_flex_flow(ui.todo_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.todo_host, 14, 0);
    lv_obj_clear_flag(ui.todo_host, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // How to get out of standby, at the bottom.
    auto* hint = ValetLabel(root, std::strcmp(Lang::Current(), "zh-CN") == 0
        ? "按电源键醒来 · 长按 AI 唤醒键，醒来直接提问" : "Power key wakes · hold the AI key to wake and ask", ValetSmall());
    lv_obj_set_width(hint, width - 2 * kValetSide);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, kValetSide, -44);
}

void StandbyClassic_StartOverlayRuntime() {
    StandbyClassic_RefreshDateOnce();
    StandbyClassic_ApplyWeatherUi();
    StandbyClassic_ApplyTodosFromCache();
    StandbyClassic_ScheduleWeatherEnsure();
}

void StandbyClassic_Build(lv_obj_t* root, bool as_overlay) {
    if (root == nullptr) {
        return;
    }
    StandbyClassic_as_overlay = as_overlay;
    StandbyClassic_alive = true;
    StandbyClassic_State() = StandbyClassicUiState{};
    StandbyClassic_BuildUi(root);
}

void StandbyClassic_StartRuntime() {
    if (!StandbyClassic_alive) {
        return;
    }
    StandbyClassic_StartOverlayRuntime();
}

void StandbyClassic_StopRuntime() {}

void StandbyClassic_Teardown() {
    StandbyClassic_StopRuntime();
    StandbyClassic_alive = false;
    StandbyClassic_as_overlay = false;
    StandbyClassic_todos.clear();
    StandbyClassic_State() = StandbyClassicUiState{};
}
