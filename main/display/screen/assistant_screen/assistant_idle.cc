#include "assistant_fonts.h"
#include "assets/lang_config.h"
#include "chat_cloud.h"
#include "fontpack_lvgl.h"
#include "home_screen/home_valet_font.h"
#include "valet_reading_context.h"

#include <cstring>
#include <algorithm>

namespace {
// Layout coordinates use a 480 x 800 reference; content is editable independently
// of the resource partition. Icons are LVGL strokes based on the supplied artwork.
constexpr int kReferenceWidth = 480;
constexpr int kReferenceHeight = 800;
constexpr int kTalkRightMargin = 16;
constexpr int kRuleWidth = 2;

bool IsEnglish() {
    return Lang::CODE && std::strcmp(Lang::CODE, "en-US") == 0;
}

struct Feature {
    const char* title_zh;
    const char* detail_zh;
    const char* title_en;
    const char* detail_en;
};
constexpr Feature kFeatures[] = {
    {"记录待办", "帮我把重要的事项整理成待办", "To-do list", "Organize important things into tasks"},
    {"旅游助手", "攻略、美食、天气、交通，一键规划", "Travel assistant", "Plan food, weather and transport"},
    {"探索世界", "解答各种疑问，探索未知", "Explore the world", "Ask questions and discover more"},
    {"陪伴聊天", "放松交流、分享心情、打发时间", "Let's chat", "Relax, share feelings and pass the time"},
};

void DrawIcon(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    lv_layer_t* layer = lv_event_get_layer(e);
    const int icon_size = lv_obj_get_width(obj);
    auto scale = [icon_size](int value) { return value * icon_size / 52; };
    const int kind = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto line = [&](int x1, int y1, int x2, int y2) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.color = lv_color_black();
        d.width = std::max(2, scale(3));
        d.round_start = d.round_end = true;
        d.p1.x = bounds.x1 + scale(x1); d.p1.y = bounds.y1 + scale(y1);
        d.p2.x = bounds.x1 + scale(x2); d.p2.y = bounds.y1 + scale(y2);
        lv_draw_line(layer, &d);
    };
    auto arc = [&](int x, int y, int radius, int start, int end, int width = 3) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = lv_color_black(); d.width = std::max(2, scale(width));
        d.center = {bounds.x1 + scale(x), bounds.y1 + scale(y)};
        d.radius = scale(radius); d.start_angle = start; d.end_angle = end;
        lv_draw_arc(layer, &d);
    };
    auto box = [&](int x, int y, int w, int h) {
        line(x,y,x+w,y); line(x+w,y,x+w,y+h);
        line(x+w,y+h,x,y+h); line(x,y+h,x,y);
    };
    switch (kind) {
        case 0:
            arc(25,22,17,0,360); arc(25,22,12,240,320);
            line(20,43,30,43); line(22,48,28,48);
            line(4,7,8,9); line(3,14,7,15);
            line(42,9,46,7); line(43,15,47,14);
            break;
        case 1:
            box(8,4,13,41); box(21,14,22,31);
            line(33,8,43,8); line(43,8,43,45);
            for (int y=20; y<42; y+=7) line(27,y,33,y);
            break;
        case 2:
            line(18,6,45,6); line(45,6,45,27); line(45,27,40,27);
            line(6,16,36,16); line(36,16,36,38); line(36,38,23,38);
            line(23,38,17,44); line(17,44,17,38);
            line(17,38,6,38); line(6,38,6,16);
            arc(21,25,5,180,360); line(26,25,21,30); line(21,30,21,32);
            line(21,35,21,35);
            break;
        case 3:
            arc(13,15,11,0,360); arc(39,15,11,0,360,11);
            arc(23,42,15,180,265); arc(39,42,13,180,360);
            break;
    }
}

lv_obj_t* Label(lv_obj_t* host, const char* text, int x, int y, int width,
                const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(host);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(label, 3, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return label;
}

lv_obj_t* PlainObject(lv_obj_t* parent, int width, int height) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_clear_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return obj;
}

void Rule(lv_obj_t* parent, int x, int y, int width, int height) {
    lv_obj_t* rule = PlainObject(parent, width, height);
    lv_obj_set_pos(rule, x, y);
    lv_obj_set_style_bg_color(rule, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
}
}  // namespace

const char* AssistantScreen::DisplayName(bool compact) {
    const bool en = IsEnglish();
    switch (chat_cloud::ActiveProvider()) {
        case chat_cloud::Provider::Custom:
            return en ? "Custom assistant" : "自建助理";
        case chat_cloud::Provider::Xiaozhi:
            return en ? (compact ? "XZ AI" : "Xiaozhi AI") : "小智AI";
        case chat_cloud::Provider::Yunxin:
            return en ? (compact ? "NE AI" : "NetEase AI") : "网易AI";
        default:
            return en ? (compact ? "BW AI" : "Baiwen AI") : "百问AI";
    }
}

void Assistant_HideIdleHint() {
    lv_obj_t* hint = Assistant_State().chrome.idle_hint;
    if (hint != nullptr && lv_obj_is_valid(hint)) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void Assistant_ShowIdleHint() {
    lv_obj_t* hint = Assistant_State().chrome.idle_hint;
    if (hint != nullptr && lv_obj_is_valid(hint)) {
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// Welcome page in the Valet design: a headline that says what this page is
// for, one line on how to use it, and what it can do listed above the talk bar.
// `host` covers the screen; the header band on top and the talk bar at the
// bottom are drawn by the screen, so this page only fills the area between.
void Assistant_BuildIdleHint(lv_obj_t* host) {
    const bool en = IsEnglish();
    const bool custom = chat_cloud::IsCustom();
    const bool book = custom && valet::SelectedReadingContext().Valid();
    lv_obj_update_layout(host);
    const int width = lv_obj_get_width(host);
    const int height = lv_obj_get_height(host);
    constexpr int kSide = 33;
    const lv_font_t* small = fontpack_lv_font_get(25, 2);
    if (!small) small = fontpack_lv_font_ui();
    const lv_font_t* regular = fontpack_lv_font_ui();
    const lv_font_t* bold = Assistant_UiFontBold();
    lv_obj_set_style_bg_color(host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);

    const int top = Assistant_ValetBodyTop();
    lv_obj_t* headline = Label(host, book ? (en ? "Ask about this page" : "问这一页")
                                         : (en ? "What's on your mind?" : "想问什么？"),
                               kSide, top + 44, width - 2 * kSide, &kValetTitleFont);
    lv_obj_update_layout(headline);
    Label(host, !custom ? (en ? "Hold to talk, release to send." : "按住说话，松开发送。")
                : book ? (en ? "The page is attached; no need to read it out." : "正文已经带上，不用念给我听。")
                       : (en ? "No book attached. To-dos, weather, anything." : "不带书。待办、天气，随便聊。"),
          kSide, lv_obj_get_y(headline) + lv_obj_get_height(headline) + 14, width - 2 * kSide, small);

    // What it can do: ruled rows sitting directly on the talk bar.
    const int first = chat_cloud::ActiveProvider() == chat_cloud::Provider::Baiwen ? 0 : 1;
    const int count = 4 - first;
    constexpr int kRowH = 104;
    const int list_bottom = height - kFooterH - 14;
    const int list_top = list_bottom - count * kRowH;
    Rule(host, kSide, list_top, width - 2 * kSide, kRuleWidth);
    for (int row = 0; row < count; ++row) {
        const int i = first + row;
        lv_obj_t* item = PlainObject(host, width - 2 * kSide, kRowH);
        lv_obj_set_pos(item, kSide, list_top + row * kRowH);
        lv_obj_t* icon = PlainObject(item, 48, 48);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_add_event_cb(icon, DrawIcon, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_t* title = Label(item, en ? kFeatures[i].title_en : kFeatures[i].title_zh, 78, 0, width - 2 * kSide - 84, bold);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 78, -20);
        lv_obj_t* detail = Label(item, en ? kFeatures[i].detail_en : kFeatures[i].detail_zh, 78, 0, width - 2 * kSide - 84, small);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_align(detail, LV_ALIGN_LEFT_MID, 78, 22);
        Rule(host, kSide, list_top + (row + 1) * kRowH - 1, width - 2 * kSide, row + 1 < count ? 1 : kRuleWidth);
    }
    (void)regular;
}
