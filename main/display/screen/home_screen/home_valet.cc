#include "home_valet.h"
#include "home_screen_ui.h"
#include "home_hero.h"
#include "home_valet_font.h"
#include "app_store/app_store_icons.h"
#include "app_store_screen/app_store_screen.h"
#include "home_icon_source.h"
#include "assistant_screen/assistant_screen.h"
#include "book_screen/book_screen.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_classic.h"
#include "standby_screen/standby_screen.h"
#include "task_screen/task_screen.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "assets/lang_config.h"
#include "assistant_profiles.h"
#include "chat_cloud.h"
#include "checklist_cache.h"
#include "sd_access.h"
#include "valet_protocol.h"
#include "valet_reading_context.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <string>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// The first home page. Positions are panel pixels measured from the design in
// sim/ (新设计 → 首页, 320 px wide there, 684 here), relative to the area under
// the status bar.
namespace {
constexpr char kLog[] = "HomeValet";
constexpr int kSide = 33;                 // left and right margin
constexpr int kWidth = 684 - 2 * kSide;   // 618
constexpr int kHeroH = 284;
constexpr int kPairY = kHeroH, kPairH = 372;
constexpr int kWhoY = kPairY + kPairH + 16, kWhoH = 66;
constexpr int kBookY = kWhoY + kWhoH + 10, kBookH = 190;
constexpr int kTodoY = kBookY + kBookH + 22, kTodoH = 84;
constexpr int kFootH = 76;

struct Ui {
    lv_obj_t* root = nullptr;
    lv_obj_t* hour = nullptr;
    lv_obj_t* minute = nullptr;
    lv_obj_t* slash = nullptr;
    lv_obj_t* date = nullptr;
    lv_obj_t* lunar = nullptr;
    lv_obj_t* weather = nullptr;
    lv_obj_t* book_title = nullptr;
    lv_obj_t* book_sub = nullptr;
    lv_obj_t* book_track = nullptr;
    lv_obj_t* todo_count = nullptr;
    lv_obj_t* todo_text = nullptr;
    lv_obj_t* todo_more = nullptr;
    lv_timer_t* timer = nullptr;
    int book_progress = -1;               // 0..1000, -1 unknown
    char last_minute[8] = {};
    char last_date[32] = {};
};
Ui s;
std::atomic<bool> s_progress_loading{false};

const char* Text(const char* zh, const char* en) {
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
}
const lv_font_t* Small() { const auto* f = fontpack_lv_font_get(25, 2); return f ? f : fontpack_lv_font_ui(); }
const lv_font_t* Body() { return fontpack_lv_font_ui(); }
const lv_font_t* Strong() { const auto* f = fontpack_lv_font_get(30, 4); return f ? f : fontpack_lv_font_ui(); }

void Ask(lv_event_t*) { valet::SelectStandaloneContext(); ValetProtocol::SetLive(false); AssistantScreen::RequestOpen(); }
void Call(lv_event_t*) { valet::SelectStandaloneContext(); AssistantScreen::RequestLive(); }
void Read(lv_event_t*) { ScreenNavigateTo(BookScreen::CreateLatest); }
void Tasks(lv_event_t*) { ScreenNavigateTo(TaskScreen::Create); }
void OpenAssistantSettings(lv_event_t*) { ScreenNavigateTo(SettingsScreen::CreateAssistant); }
void Apps(lv_event_t*) { Home_page = 1; ScreenNavigateTo(HomeScreen::Create); }

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, lv_event_cb_t click = nullptr) {
    auto* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (click) {
        lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(box, click, LV_EVENT_CLICKED, nullptr);
        HapticAttachClick(box);
    } else {
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    }
    return box;
}
lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font, bool white = false) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, white ? lv_color_white() : lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}
void Line(lv_layer_t* layer, int x1, int y1, int x2, int y2, int width, bool white = false, bool round = false) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = white ? lv_color_white() : lv_color_black();
    line.width = width;
    line.opa = LV_OPA_COVER;
    line.round_start = line.round_end = round;
    line.p1 = {static_cast<lv_value_precise_t>(x1), static_cast<lv_value_precise_t>(y1)};
    line.p2 = {static_cast<lv_value_precise_t>(x2), static_cast<lv_value_precise_t>(y2)};
    lv_draw_line(layer, &line);
}
void Quad(lv_layer_t* layer, const lv_point_precise_t* p, bool black) {
    lv_draw_triangle_dsc_t fill;
    lv_draw_triangle_dsc_init(&fill);
    fill.color = black ? lv_color_black() : lv_color_white();
    fill.opa = LV_OPA_COVER;
    fill.p[0] = p[0]; fill.p[1] = p[1]; fill.p[2] = p[2];
    lv_draw_triangle(layer, &fill);
    fill.p[1] = p[2]; fill.p[2] = p[3];
    lv_draw_triangle(layer, &fill);
}
// "go" chevron, drawn because the UI font pack has no arrow glyphs.
void DrawChevron(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const intptr_t flags = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const bool white = (flags & 1) != 0;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = (a.y2 - a.y1) / 2 - 2;
    const int d = (flags & 2) ? -r / 2 : r / 2; // bit 1: points left
    Line(lv_event_get_layer(e), cx - d, cy - r, cx + d, cy, 2, white, true);
    Line(lv_event_get_layer(e), cx + d, cy, cx - d, cy + r, 2, white, true);
}
lv_obj_t* Chevron(lv_obj_t* parent, int size, bool white = false, bool left = false) {
    auto* icon = Box(parent, 0, 0, size, size);
    lv_obj_add_event_cb(icon, DrawChevron, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void*>(static_cast<intptr_t>((white ? 1 : 0) | (left ? 2 : 0))));
    return icon;
}

// ── Clock and date
void DrawClockSlash(lv_event_t* e) {
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    Line(lv_event_get_layer(e), a.x1, a.y2 - 2, a.x2, a.y1 + 2, 2);
}
void RefreshClock(bool force) {
    const time_t now = std::time(nullptr);
    tm local{};
    const bool valid = localtime_r(&now, &local) != nullptr && local.tm_year >= 120;
    char hour[4] = "00", minute[4] = "00", date[32] = "", key[8];
    if (valid) {
        std::snprintf(hour, sizeof(hour), "%02d", local.tm_hour);
        std::snprintf(minute, sizeof(minute), "%02d", local.tm_min);
        std::snprintf(date, sizeof(date), "%04d.%02d.%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    } else {
        std::snprintf(date, sizeof(date), "%s", Lang::Strings::HOME_DATE_SLASH_PLACEHOLDER);
    }
    std::snprintf(key, sizeof(key), "%s%s", hour, minute);
    if (force || std::strcmp(key, s.last_minute) != 0) {
        lv_label_set_text(s.hour, hour);
        lv_label_set_text(s.minute, minute);
        std::snprintf(s.last_minute, sizeof(s.last_minute), "%s", key);
    }
    if (force || std::strcmp(date, s.last_date) != 0) {
        lv_label_set_text(s.date, date);
        std::snprintf(s.last_date, sizeof(s.last_date), "%s", date);
    }
}
void RefreshWeather() {
    if (!s.root) return;
    const time_t now = std::time(nullptr);
    tm local{};
    const bool valid = localtime_r(&now, &local) != nullptr && local.tm_year >= 120;
    static const char* const kZh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    static const char* const kEn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const bool zh = std::strcmp(Lang::Current(), "zh-CN") == 0;
    StandbyClassicWeatherView view;
    const bool weather = StandbyClassic_CopyWeatherView(&view) && view.valid;
    char lunar[24] = {};
    if (weather && zh) StandbyClassic_FormatLunarOrWeekday(lunar, sizeof(lunar), view.lunar);
    lv_label_set_text(s.lunar, lunar);
    if (lunar[0]) lv_obj_remove_flag(s.lunar, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s.lunar, LV_OBJ_FLAG_HIDDEN);
    char line[80] = {};
    const char* day = valid ? (zh ? kZh : kEn)[local.tm_wday] : "";
    if (weather && view.has_temp) std::snprintf(line, sizeof(line), "%s · %s %d°", day, view.text, view.temp);
    else if (weather) std::snprintf(line, sizeof(line), "%s · %s", day, view.text);
    else std::snprintf(line, sizeof(line), "%s", day);
    lv_label_set_text(s.weather, line);
}
void BuildHero(lv_obj_t* root) {
    // Hours over minutes, minutes shifted right, a thin rising stroke between them.
    s.hour = Label(root, "00", &kValetClockFont);
    lv_obj_set_pos(s.hour, kSide + 14, 20);
    s.minute = Label(root, "00", &kValetClockFont);
    lv_obj_set_pos(s.minute, kSide + 14, 156);
    s.slash = Box(root, kSide + 16, 124, 154, 34);
    lv_obj_add_event_cb(s.slash, DrawClockSlash, LV_EVENT_DRAW_MAIN, nullptr);

    // Date block: right-aligned, sitting on the bottom of the hero.
    auto* column = Box(root, 0, 0, 330, LV_SIZE_CONTENT);
    lv_obj_align(column, LV_ALIGN_TOP_RIGHT, -kSide, 0);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(column, 6, 0);
    auto* rule = Box(column, 0, 0, 84, 2);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_bottom(rule, 8, 0);
    s.date = Label(column, "", Small());
    s.lunar = Label(column, "", Strong());
    s.weather = Label(column, "", Small());
    lv_obj_update_layout(column);
    lv_obj_set_y(column, kHeroH - 20 - lv_obj_get_height(column));
}

// ── Ask / Call
void DrawChoice(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const intptr_t kind = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const bool ask = kind != 0, alone = kind == 2;
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    // Design polygons on a 222 x 134 grid: ask (0,0)(127,0)(96,134)(0,134), call (134,0)(222,0)(222,134)(103,134).
    // Without a call entry Ask takes the whole row and keeps the slant on its right: (0,0)(222,0)(190,134)(0,134).
    const int w = a.x2 - a.x1, h = a.y2 - a.y1;
    auto px = [&](int v) { return static_cast<lv_value_precise_t>(a.x1 + v * w / 222); };
    const lv_value_precise_t top = a.y1, bottom = a.y1 + h;
    if (ask) {
        const lv_point_precise_t p[4] = {{px(0), top}, {px(alone ? 222 : 127), top}, {px(alone ? 190 : 96), bottom}, {px(0), bottom}};
        Quad(layer, p, true);
        // Microphone: capsule, cradle, stem.
        const int ix = a.x1 + 60, iy = a.y1 + 32;
        lv_draw_rect_dsc_t capsule;
        lv_draw_rect_dsc_init(&capsule);
        capsule.bg_opa = LV_OPA_TRANSP;
        capsule.border_color = lv_color_white();
        capsule.border_width = 5;
        capsule.border_opa = LV_OPA_COVER;
        capsule.radius = LV_RADIUS_CIRCLE;
        const lv_area_t body = {ix - 10, iy, ix + 10, iy + 42};
        lv_draw_rect(layer, &capsule, &body);
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = 5;
        arc.opa = LV_OPA_COVER;
        arc.rounded = 1;
        arc.center = {static_cast<lv_value_precise_t>(ix), static_cast<lv_value_precise_t>(iy + 27)};
        arc.radius = 22;
        arc.start_angle = 0;
        arc.end_angle = 180;
        lv_draw_arc(layer, &arc);
        Line(layer, ix, iy + 50, ix, iy + 60, 5, true, true);
    } else {
        const lv_point_precise_t p[4] = {{px(134), top}, {px(222), top}, {px(222), bottom}, {px(103), bottom}};
        for (int i = 0; i < 4; ++i)
            Line(layer, p[i].x, p[i].y, p[(i + 1) % 4].x, p[(i + 1) % 4].y, 3);
        // Sound wave: five bars.
        const int ix = a.x2 - 60, iy = a.y1 + 62;
        static const int kHalf[5] = {7, 18, 30, 18, 7};
        for (int i = 0; i < 5; ++i)
            Line(layer, ix + (i - 2) * 13, iy - kHalf[i], ix + (i - 2) * 13, iy + kHalf[i], 5, false, true);
    }
}
void BuildPair(lv_obj_t* root) {
    auto* pair = Box(root, kSide, kPairY, kWidth, kPairH);
    // No assistant of your own, no Call: Ask alone, across the whole row.
    const bool call = chat_cloud::HasCallEntry();
    for (int i = 0; i < (call ? 2 : 1); ++i) {
        const bool ask = i == 0;
        // Each half owns its side of the cut; the art is drawn over the whole pair.
        auto* art = Box(pair, 0, 0, kWidth, kPairH);
        lv_obj_add_event_cb(art, DrawChoice, LV_EVENT_DRAW_MAIN,
                            reinterpret_cast<void*>(static_cast<intptr_t>(!ask ? 0 : call ? 1 : 2)));
        auto* hit = Box(pair, ask ? 0 : kWidth / 2 + 8, 0, ask && !call ? kWidth : kWidth / 2 - 8, kPairH,
                        ask ? Ask : Call);
        auto* title = Label(hit, ask ? Text("提问", "Ask") : Text("通话", "Call"), &kValetTitleFont, ask);
        lv_obj_set_style_text_letter_space(title, 4, 0);
        auto* note = Label(hit, ask ? Text("按住，说一句", "Hold and say it") : Text("接通就聊", "Just talk"), Small(), ask);
        if (ask) {
            lv_obj_align(title, LV_ALIGN_BOTTOM_LEFT, 36, -70);
            lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 36, -30);
        } else {
            lv_obj_align(title, LV_ALIGN_BOTTOM_RIGHT, -32, -70);
            lv_obj_align(note, LV_ALIGN_BOTTOM_RIGHT, -36, -30);
        }
    }
}

// ── Who answers
void BuildWho(lv_obj_t* root) {
    std::string who;
    bool ready = true;
    if (chat_cloud::IsCustom()) {
        const auto profiles = valet::LoadAssistantProfiles();
        if (const auto* active = profiles.Active()) who = active->name;
        else { who = Text("自建 · 还没配对", "Custom · not paired"); ready = false; }
    } else {
        who = std::string(AssistantScreen::DisplayName()) + Text(" · 官方订阅", " · official plan");
    }
    auto* row = Box(root, kSide, kWhoY, kWidth, kWhoH, OpenAssistantSettings);
    auto* dot = Box(row, 3, 0, 18, 18);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 3, 0);
    lv_obj_set_style_border_color(dot, lv_color_black(), 0);
    lv_obj_set_style_bg_color(dot, ready ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    auto* label = Label(row, who.c_str(), Small());
    lv_obj_set_width(label, kWidth - 90);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 38, 0);
    lv_obj_align(Chevron(row, 24), LV_ALIGN_RIGHT_MID, 0, 0);
}

// ── Continue reading
void DrawBookCard(lv_event_t* e) {
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    // Dark wedge on the right, cut on the same slant as the two blocks above.
    const lv_point_precise_t wedge[4] = {
        {static_cast<lv_value_precise_t>(a.x2 - 152), static_cast<lv_value_precise_t>(a.y1)},
        {static_cast<lv_value_precise_t>(a.x2), static_cast<lv_value_precise_t>(a.y1)},
        {static_cast<lv_value_precise_t>(a.x2), static_cast<lv_value_precise_t>(a.y2)},
        {static_cast<lv_value_precise_t>(a.x2 - 208), static_cast<lv_value_precise_t>(a.y2)}};
    Quad(layer, wedge, true);
    // A blank cover: outline with a thick spine.
    const int cx = a.x1 + 22, cy = a.y1 + 30, cw = 88, ch = (a.y2 - a.y1) - 60;
    lv_draw_rect_dsc_t cover;
    lv_draw_rect_dsc_init(&cover);
    cover.bg_opa = LV_OPA_TRANSP;
    cover.border_color = lv_color_black();
    cover.border_width = 2;
    cover.border_opa = LV_OPA_COVER;
    const lv_area_t outline = {cx, cy, cx + cw, cy + ch};
    lv_draw_rect(layer, &cover, &outline);
    Line(layer, cx + 3, cy, cx + 3, cy + ch, 7);
    for (int i = 0; i < 3; ++i) Line(layer, cx + 24, cy + 34 + i * 18, cx + cw - 16 - i * 14, cy + 34 + i * 18, 2);
}
std::string BookTitle(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}
void LoadProgress(void* arg) {
    auto* path = static_cast<std::string*>(arg);
    {
        sd_access::Transaction transaction;
        if (transaction)
            reader::book_progress_cache::Upsert(path->c_str(), reader::BookSession::PeekProgress(path->c_str()));
    }
    delete path;
    s_progress_loading.store(false);
    vTaskDelete(nullptr);
}
void RefreshBook() {
    if (!s.book_sub) return;
    const std::string path = reader::book_home_snapshot::LoadLatestAbsPath();
    reader::BookSession::ProgressPeek peek;
    const bool known = !path.empty() && reader::book_progress_cache::TryGet(path.c_str(), peek);
    // The progress file is on the SD card; read it once, off the UI task.
    if (!path.empty() && !known && !s_progress_loading.exchange(true)) {
        auto* work = new (std::nothrow) std::string(path);
        if (!work || xTaskCreate(LoadProgress, "valet_progress", 6144, work, 4, nullptr) != pdPASS) {
            delete work;
            s_progress_loading.store(false);
            ESP_LOGE(kLog, "reading progress task creation failed");
        }
    }
    const int progress = known && peek.progress_x10 >= 0 ? std::clamp(peek.progress_x10, 0, 1000) : -1;
    if (progress == s.book_progress) return;
    s.book_progress = progress;
    char text[40];
    if (progress >= 0) std::snprintf(text, sizeof(text), Text("读到 %d.%d%%", "%d.%d%% read"), progress / 10, progress % 10);
    else std::snprintf(text, sizeof(text), "%s", Text("接着上次的位置", "Where you left off"));
    lv_label_set_text(s.book_sub, text);
    lv_obj_set_width(s.book_track, std::max(0, progress) * 236 / 1000);
}
void BuildBook(lv_obj_t* root) {
    const std::string path = reader::book_home_snapshot::LoadLatestAbsPath();
    auto* card = Box(root, kSide, kBookY, kWidth, kBookH, Read);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_add_event_cb(card, DrawBookCard, LV_EVENT_DRAW_MAIN, nullptr);
    const std::string title = path.empty() ? Text("还没有在读的书", "Nothing open yet") : BookTitle(path);
    s.book_title = Label(card, title.c_str(), Strong());
    lv_obj_set_width(s.book_title, 268);
    lv_label_set_long_mode(s.book_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s.book_title, 138, 30);
    s.book_sub = Label(card, path.empty() ? Text("去书架挑一本", "Pick one from the shelf") : "", Small());
    lv_obj_set_width(s.book_sub, 268);
    lv_label_set_long_mode(s.book_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s.book_sub, 138, 84);
    auto* track = Box(card, 138, 134, 240, 14);
    lv_obj_set_style_border_width(track, 2, 0);
    lv_obj_set_style_border_color(track, lv_color_black(), 0);
    s.book_track = Box(track, 0, 0, 0, 10);
    lv_obj_set_style_bg_color(s.book_track, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.book_track, LV_OPA_COVER, 0);
    if (path.empty()) lv_obj_add_flag(track, LV_OBJ_FLAG_HIDDEN);
    auto* go = Label(card, path.empty() ? Text("去书架", "Shelf") : Text("接着读", "Read"), Strong(), true);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_align(Chevron(card, 26, true), LV_ALIGN_RIGHT_MID, -12, 0);
    s.book_progress = -2;
    if (!path.empty()) RefreshBook();
}

// ── To-do
void RefreshTodo() {
    if (!s.todo_text) return;
    // Items live in PSRAM; this task's stack is too small for a copy of the list.
    constexpr size_t kMax = 2;
    auto* items = static_cast<checklist_cache_item_t*>(
        heap_caps_calloc(kMax, sizeof(checklist_cache_item_t), MALLOC_CAP_SPIRAM));
    size_t pending = 0, done = 0, today = 0;
    const bool ready = checklist_cache_counts(nullptr, &today, &done, &pending);
    const size_t copied = items && ready ? checklist_cache_copy_pending(items, kMax) : 0;
    char count[8];
    std::snprintf(count, sizeof(count), "%u", static_cast<unsigned>(ready ? pending : 0));
    lv_label_set_text(s.todo_count, count);
    lv_label_set_text(s.todo_text, copied ? items[0].title :
        ready ? Text("今天没有待办", "Nothing to do today") : Text("今天的待办", "Today's to-dos"));
    char more[24] = {};
    if (pending > 1) std::snprintf(more, sizeof(more), Text("还有 %u 条", "+%u"), static_cast<unsigned>(pending - 1));
    lv_label_set_text(s.todo_more, more);
    heap_caps_free(items);
}
void BuildTodo(lv_obj_t* root) {
    auto* row = Box(root, kSide, kTodoY, kWidth, kTodoH, Tasks);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    auto* badge = Box(row, 3, 0, 44, 44);
    lv_obj_align(badge, LV_ALIGN_LEFT_MID, 3, -2);
    lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    s.todo_count = Label(badge, "0", Small(), true);
    lv_obj_center(s.todo_count);
    s.todo_text = Label(row, "", Body());
    lv_obj_set_width(s.todo_text, 360);
    lv_label_set_long_mode(s.todo_text, LV_LABEL_LONG_DOT);
    lv_obj_align(s.todo_text, LV_ALIGN_LEFT_MID, 70, -2);
    s.todo_more = Label(row, "", Small());
    lv_obj_align(s.todo_more, LV_ALIGN_RIGHT_MID, -44, -2);
    lv_obj_align(Chevron(row, 24), LV_ALIGN_RIGHT_MID, 0, -2);
    RefreshTodo();
}

// ── Footer: which page, and the way to the other one
void Today(lv_event_t*) { Home_page = 0; ScreenNavigateTo(HomeScreen::Create); }
void BuildFoot(lv_obj_t* root, int height) {
    auto* foot = Box(root, kSide, height - kFootH, kWidth, kFootH);
    const int pages = Home_PageCount();
    for (int i = 0; i < pages; ++i) {
        auto* dot = Box(foot, i * 30, 0, 14, 14);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, i * 30, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        lv_obj_set_style_border_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_color(dot, i == Home_page ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    }
    const bool first = Home_page == 0;
    auto* link = Box(foot, 0, 0, 220, kFootH, first ? Apps : Today);
    lv_obj_align(link, LV_ALIGN_RIGHT_MID, 0, 0);
    if (first) {
        lv_obj_align(Label(link, Text("全部应用", "All apps"), Small()), LV_ALIGN_RIGHT_MID, -34, 0);
        lv_obj_align(Chevron(link, 24), LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        auto* label = Label(link, Text("今天", "Today"), Small());
        lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_update_layout(label);
        lv_obj_align(Chevron(link, 24, false, true), LV_ALIGN_RIGHT_MID, -lv_obj_get_width(label) - 10, 0);
    }
}

// ── App pages: a title, two columns of four, the same footer
constexpr int kAppsTop = 100, kCellW = 298, kCellH = 195, kCellGapX = 22, kCellGapY = 22;
// Three books and a board, on the 24-unit grid the design draws its icons on.
void DrawShelfIcon(lv_event_t* e) {
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    const int w = a.x2 - a.x1;
    auto x = [&](int tenths) { return a.x1 + tenths * w / 240; };
    auto y = [&](int tenths) { return a.y1 + tenths * w / 240; };
    auto* layer = lv_event_get_layer(e);
    const bool white = lv_event_get_user_data(e) != nullptr;
    Line(layer, x(50), y(40), x(50), y(195), 5, white, true);
    Line(layer, x(95), y(40), x(95), y(195), 5, white, true);
    Line(layer, x(135), y(55), x(180), y(195), 5, white, true);
    Line(layer, x(30), y(200), x(210), y(200), 5, white, true);
}
void BuildAppName(lv_obj_t* cell, const AppEntry& entry, int style) {
    auto* name = Label(cell, Home_AppLabel(entry), Small());
    lv_obj_set_width(name, kCellW - 24);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    if (style == HomeScreen::kCardStyleGray) {
        // Text on a dot pattern is unreadable; give the name a plain backing.
        lv_obj_set_width(name, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(name, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(name, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(name, 8, 0);
    }
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -28);}
// Built-in icons are 120 px, stored compressed in strips that cannot be scaled
// while drawing: decode the whole icon once, then scale it. App store icons are
// 128 px one-bit images with their own resampler. Returns the icon, centred on
// (cx, cy) inside `parent`.
lv_obj_t* AppIcon(lv_obj_t* parent, const AppEntry& entry, int edge, int cx, int cy, bool white) {
    if (entry.icon_path && std::strcmp(entry.icon_path, kHomeValetShelfIcon) == 0) {
        auto* art = Box(parent, cx - edge / 2, cy - edge / 2, edge, edge);
        lv_obj_add_event_cb(art, DrawShelfIcon, LV_EVENT_DRAW_MAIN, white ? reinterpret_cast<void*>(1) : nullptr);
        return art;
    }
    auto* icon = lv_image_create(parent);
    int box = edge;
    if (entry.installed_id.empty() && entry.create != AppStoreScreen::Create &&
        HomeIconSetFullSource(icon, entry.icon_path)) {
        const int natural = lv_image_get_src_width(icon);
        if (natural > 0 && natural != edge) {
            box = natural;
            lv_obj_set_size(icon, natural, natural);
            lv_image_set_pivot(icon, natural / 2, natural / 2);
            lv_image_set_scale(icon, edge * LV_SCALE_NONE / natural);
        }
    } else {
        AppStoreIcons::SetSource(icon, Home_AppIcon(entry), edge);
        lv_obj_set_size(icon, edge, edge);
    }
    lv_obj_set_style_image_recolor(icon, white ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(icon, cx - box / 2, cy - box / 2);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    return icon;
}

// Slanted tiles, from the design's 246-unit grid (x 2.78): rows of 206 px every
// 228; the cut leans one way on even rows and the other on odd rows, and dark
// and light alternate like a checkerboard.
constexpr int kTileH = 206, kTilePitch = 228, kTileL = 35, kTileR = 649, kTileNear = 267, kTileFar = 389, kTileGap = 20;
void DrawTile(lv_event_t* e) {
    const intptr_t flags = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const bool right = flags & 1, dark = flags & 2, odd = flags & 4;
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    const int top = odd ? kTileNear : kTileFar, bottom = odd ? kTileFar : kTileNear;
    auto P = [&](int x, bool low) {
        return lv_point_precise_t{static_cast<lv_value_precise_t>(a.x1 + x),
                                  static_cast<lv_value_precise_t>(low ? a.y2 - 1 : a.y1 + 1)};
    };
    const lv_point_precise_t p[4] = {
        P(right ? top + kTileGap : kTileL, false), P(right ? kTileR : top, false),
        P(right ? kTileR : bottom, true), P(right ? bottom + kTileGap : kTileL, true)};
    auto* layer = lv_event_get_layer(e);
    if (dark) { Quad(layer, p, true); return; }
    for (int i = 0; i < 4; ++i) Line(layer, p[i].x, p[i].y, p[(i + 1) % 4].x, p[(i + 1) % 4].y, 3);
}
void BuildTiles(lv_obj_t* root, int start, int total) {
    // All the shapes first, so no tile is painted over its neighbour's icon or name.
    for (int i = 0; i < kHomeValetAppsPerPage && start + i < total; ++i) {
        const int row = i / 2, side = i % 2;
        auto* art = Box(root, 0, kAppsTop + row * kTilePitch, LV_HOR_RES, kTileH);
        lv_obj_add_event_cb(art, DrawTile, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(static_cast<intptr_t>(
            side | (((row + side) % 2 == 0) ? 2 : 0) | ((row % 2) ? 4 : 0))));
    }
    // The two tiles of a row share the slanted edge; split the touch area down its middle.
    constexpr int kSplit = (kTileNear + kTileFar + kTileGap) / 2;
    for (int i = 0; i < kHomeValetAppsPerPage && start + i < total; ++i) {
        const AppEntry& entry = kValetApps[start + i];
        const int row = i / 2, side = i % 2;
        const bool dark = (row + side) % 2 == 0;
        const int x = side ? kSplit : kTileL, w = side ? kTileR - kSplit : kSplit - kTileL;
        auto* hit = Box(root, x, kAppsTop + row * kTilePitch, w, kTileH);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(hit);
        lv_obj_set_user_data(hit, const_cast<AppEntry*>(&entry));
        lv_obj_add_event_cb(hit, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry*>(&entry));
        const char* text = Home_AppLabel(entry);
        const bool long_name = std::strlen(text) > 15 || (static_cast<unsigned char>(text[0]) < 0x80 && std::strlen(text) > 5);
        constexpr int kIcon = 89;
        // Icon on the outer side, name towards the cut.
        const int icon_x = side ? 586 - x : (long_name ? 92 : 108) - x;
        AppIcon(hit, entry, kIcon, icon_x, kTileH / 2, dark);
        auto* name = Label(hit, text, long_name ? Small() : Body(), dark);
        if (side) lv_obj_align(name, LV_ALIGN_RIGHT_MID, -(kTileR - 525), 0);
        else lv_obj_align(name, LV_ALIGN_LEFT_MID, (long_name ? 150 : 167) - x, 0);
    }
}
void BuildAppCell(lv_obj_t* root, int slot, const AppEntry& entry, int style) {
    auto* cell = Box(root, kSide + (slot % 2) * (kCellW + kCellGapX), kAppsTop + (slot / 2) * (kCellH + kCellGapY),
                     kCellW, kCellH);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cell);
    lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    lv_obj_add_event_cb(cell, Home_OnAppClicked, LV_EVENT_CLICKED, const_cast<AppEntry*>(&entry));
    Home_ApplyCardStyle(cell, style);
    if (style == HomeScreen::kCardStyleBorder) lv_obj_set_style_border_width(cell, 3, 0);
    AppIcon(cell, entry, 78, kCellW / 2, 28 + 39, false);
    BuildAppName(cell, entry, style);
}

void OnTimer(lv_timer_t*) {
    if (!s.root || StandbyScreen::IsActive() || lv_obj_get_screen(s.root) != lv_screen_active()) return;
    RefreshClock(false);
    RefreshWeather();
    RefreshBook();
    RefreshTodo();
}
void OnDeleted(lv_event_t* e) {
    if (lv_event_get_target(e) != s.root) return;
    StandbyClassic_RemoveWeatherUiListener(RefreshWeather);
    if (s.timer) lv_timer_delete(s.timer);
    s = Ui{};
}
}

void HomeValet_Build(lv_obj_t* screen, lv_coord_t status_height) {
    HomeHero_Teardown();
    if (s.timer) lv_timer_delete(s.timer);
    StandbyClassic_RemoveWeatherUiListener(RefreshWeather);
    s = Ui{};
    const int height = LV_VER_RES - status_height;
    s.root = Box(screen, 0, status_height, LV_HOR_RES, height);
    lv_obj_add_event_cb(s.root, OnDeleted, LV_EVENT_DELETE, nullptr);
    BuildHero(s.root);
    BuildPair(s.root);
    BuildWho(s.root);
    BuildBook(s.root);
    BuildTodo(s.root);
    BuildFoot(s.root, height);
    RefreshClock(true);
    RefreshWeather();
    StandbyClassic_AddWeatherUiListener(RefreshWeather);
    StandbyClassic_RequestWeatherEnsure();
    s.timer = lv_timer_create(OnTimer, 1000, nullptr);
}

void HomeValet_BuildApps(lv_obj_t* screen, lv_coord_t status_height, int style) {
    HomeHero_Teardown();
    if (s.timer) lv_timer_delete(s.timer);
    StandbyClassic_RemoveWeatherUiListener(RefreshWeather);
    s = Ui{};
    const int height = LV_VER_RES - status_height;
    auto* root = Box(screen, 0, status_height, LV_HOR_RES, height);
    auto* title = Label(root, Text("全部应用", "All apps"), Strong());
    lv_obj_set_pos(title, kSide, 20);
    const int total = static_cast<int>(kValetApps.size());
    const int start = (Home_page - 1) * kHomeValetAppsPerPage;
    if (style == HomeScreen::kCardStyleSlash) BuildTiles(root, start, total);
    else for (int i = 0; i < kHomeValetAppsPerPage && start + i < total; ++i) BuildAppCell(root, i, kValetApps[start + i], style);
    BuildFoot(root, height);
    ESP_LOGI(kLog, "apps page %d/%d apps %d..%d of %d", Home_page + 1, Home_PageCount(), start,
             std::min(start + kHomeValetAppsPerPage, total) - 1, total);
}
