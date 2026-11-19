#include "home_valet.h"
#include "home_screen_ui.h"
#include "home_hero.h"
#include "home_icon_source.h"
#include "assistant_screen/assistant_screen.h"
#include "book_screen/book_screen.h"
#include "settings_screen/settings_screen.h"
#include "task_screen/task_screen.h"
#include "reader/book_home_snapshot.h"
#include "assets/lang_config.h"
#include "chat_cloud.h"
#include "valet_protocol.h"
#include "valet_reading_context.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "ui_scale.h"

namespace {
const char* Text(const char* zh, const char* en) {
    return std::string(Lang::Current()) == "zh-CN" ? zh : en;
}
void Ask(lv_event_t*) { valet::SelectStandaloneContext(); ValetProtocol::SetLive(false); AssistantScreen::RequestOpen(); }
void Call(lv_event_t*) { valet::SelectStandaloneContext(); AssistantScreen::RequestLive(); }
void Read(lv_event_t*) { ScreenNavigateTo(BookScreen::CreateLatest); }
void Tasks(lv_event_t*) { ScreenNavigateTo(TaskScreen::Create); }
void OpenAssistantSettings(lv_event_t*) { ScreenNavigateTo(SettingsScreen::CreateAssistant); }
void Apps(lv_event_t*) { Home_page = 1; ScreenNavigateTo(HomeScreen::Create); }
lv_obj_t* Row(lv_obj_t* parent, int y, int height, const char* text, lv_event_cb_t callback) {
    auto* row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, UiSx(24), UiSy(y));
    lv_obj_set_size(row, LV_HOR_RES - UiSx(48), UiSy(height));
    lv_obj_set_style_border_width(row, UiSx(1), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, nullptr);
    HapticAttachClick(row);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(95));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, UiSx(8), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return row;
}
void DrawChoice(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const bool dark = lv_event_get_user_data(e) != nullptr;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    const int slant = UiSx(50);
    lv_point_precise_t points[4] = {
        {static_cast<lv_value_precise_t>(a.x1 + (dark ? 0 : slant)), static_cast<lv_value_precise_t>(a.y1)},
        {static_cast<lv_value_precise_t>(a.x2), static_cast<lv_value_precise_t>(a.y1)},
        {static_cast<lv_value_precise_t>(a.x2 - (dark ? slant : 0)), static_cast<lv_value_precise_t>(a.y2)},
        {static_cast<lv_value_precise_t>(a.x1), static_cast<lv_value_precise_t>(a.y2)},
    };
    lv_draw_triangle_dsc_t fill; lv_draw_triangle_dsc_init(&fill);
    fill.color = dark ? lv_color_black() : lv_color_white(); fill.opa = LV_OPA_COVER;
    fill.p[0] = points[0]; fill.p[1] = points[1]; fill.p[2] = points[2];
    lv_draw_triangle(lv_event_get_layer(e), &fill);
    fill.p[1] = points[2]; fill.p[2] = points[3];
    lv_draw_triangle(lv_event_get_layer(e), &fill);
    if (!dark) {
        lv_draw_line_dsc_t line; lv_draw_line_dsc_init(&line);
        line.color = lv_color_black(); line.width = UiSx(2); line.opa = LV_OPA_COVER;
        for (int i = 0; i < 4; ++i) { line.p1 = points[i]; line.p2 = points[(i + 1) % 4]; lv_draw_line(lv_event_get_layer(e), &line); }
    }
}
void Choice(lv_obj_t* parent, bool ask) {
    auto* button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, UiSx(ask ? 24 : 226), UiSy(275));
    lv_obj_set_size(button, UiSx(230), UiSy(210));
    lv_obj_add_event_cb(button, DrawChoice, LV_EVENT_DRAW_MAIN, ask ? reinterpret_cast<void*>(1) : nullptr);
    lv_obj_add_event_cb(button, ask ? Ask : Call, LV_EVENT_CLICKED, nullptr);
    HapticAttachClick(button);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, ask ? Text("提问\n按住，说一句", "Ask\nHold to speak") : Text("通话\n接通就聊", "Call\nJust talk"));
    lv_obj_set_style_text_color(label, ask ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, UiSx(ask ? -16 : 16), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
}
}
void HomeValet_Build(lv_obj_t* screen, lv_coord_t status_height) {
    HomeHero_MountSlash(screen, status_height);
    HomeHero_Start();
    auto* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, status_height);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status_height);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    Choice(body, true); Choice(body, false);
    std::string who = Text("选择助手", "Choose assistant");
    if (chat_cloud::IsCustom()) {
        const auto profiles = valet::LoadAssistantProfiles();
        if (const auto* active = profiles.Active()) who = active->name;
        else who = Text("自建 · 添加助理", "Custom · add assistant");
    } else who = AssistantScreen::DisplayName();
    Row(body, 500, 58, who.c_str(), OpenAssistantSettings);
    const auto latest = reader::book_home_snapshot::LoadLatestAbsPath();
    Row(body, 565, 78, latest.empty() ? Text("开始阅读", "Start reading") : Text("接着读", "Continue reading"), Read);
    Row(body, 640, 58, Text("今天的待办", "Today's tasks"), Tasks);
    Row(body, 710, 44, Text("全部应用  >", "All apps  >"), Apps);
}
