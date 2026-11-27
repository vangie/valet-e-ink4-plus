#include "book_screen/reader/book_reader_select.h"

#include "application.h"
#include "assets/lang_config.h"
#include "assistant_screen/assistant_screen.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_body.h"
#include "chat_cloud.h"
#include "epd_content_policy.h"
#include "epd_refresh.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "reader/text_encoding.h"
#include "screen_common.h"
#include "settings_screen/settings_screen.h"
#include "status_icons.h"
#include "valet_reading_context.h"

#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include <esp_log.h>

namespace {
constexpr const char* TAG = "ReaderSelect";

// 尺寸（面板像素，sim 的 2.125 倍）
constexpr int kCardSide = 17;        // 卡片离屏幕左右
constexpr int kCardRadius = 17;
constexpr int kRowMinH = 68;         // 一条预设问题
constexpr int kRowPadX = 25;
constexpr int kToolH = 102;          // 底下一排按钮
constexpr int kCardGap = 56;         // 卡片离选中的字（让出把手的圆点与扩大后的触控区）
constexpr int kScreenMargin = 16;
constexpr int kHandleBar = 4, kHandleDot = 22, kHandleW = 104, kHandleReach = 68;
constexpr int kLongPressReachX = 60; // 长按：离最近的字超过这么远（或不在这一行）就不算按在字上

const char* T(const char* zh, const char* en) { return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en; }

enum class Mode { kWord, kSentence, kFree };
enum class Drag { kNone, kGrow, kStart, kEnd };
enum Action : intptr_t { kAsk0 = 0, kAsk1, kAsk2, kLook = 10, kSentence, kAskOwn, kConnect };

std::vector<reader::GlyphBox> s_boxes;
std::vector<std::string> s_items;    // 这一页每一项正文（GlyphBox::item 指向这里）
std::vector<uint8_t> s_para;         // 每个字：是不是一段的开头

struct Selection {
    bool on = false;
    int a = 0, b = 0;                // 选中 [a, b)
    int anchor_a = 0, anchor_b = 0;  // 长按时选中的那个词
    int at = 0;                      // 长按的那个字
    Mode mode = Mode::kWord;
    Drag drag = Drag::kNone;
    int drag_dy = 0;                 // 拖把手：手指到那一行中线的竖向距离
    bool quoted = false;             // 已经拿去问了：只留反显，不出把手和卡片
    bool inverted = false;           // 位图里 [inv_a, inv_b) 现在是反着的
    int inv_a = 0, inv_b = 0;
} s;
bool s_eat_click = false;
lv_obj_t* s_card = nullptr;
lv_obj_t* s_handle[2] = {};

// ---------- 字 ----------
uint32_t Cp(int i) {
    if (i < 0 || i >= int(s_boxes.size())) return 0;
    const auto& g = s_boxes[i];
    if (g.item >= s_items.size()) return 0;
    const auto& text = s_items[g.item];
    if (g.begin >= text.size()) return 0;
    uint32_t cp = 0;
    reader::Utf8Next(reinterpret_cast<const uint8_t*>(text.data()) + g.begin, std::min<size_t>(g.len, text.size() - g.begin), &cp);
    return cp;
}
bool IsWordChar(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '\'' ||
           (cp >= 0xC0 && cp < 0x250 && cp != 0xD7 && cp != 0xF7);
}
bool IsPunct(uint32_t cp) {
    if (cp <= ' ') return true;
    if (cp < 0x80) return !IsWordChar(cp) || cp == '\'';
    return (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
           (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65);
}
bool IsEnd(uint32_t cp) {
    return cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF1B || cp == 0x2026 ||
           cp == '.' || cp == '!' || cp == '?' || cp == ';';
}
bool IsCloser(uint32_t cp) {
    return cp == 0x201D || cp == 0x2019 || cp == 0x300D || cp == 0x300F || cp == 0xFF09 || cp == ')' || cp == '"' ||
           cp == 0x300B || cp == 0x3011;
}
// 第 i 个字之后句子结束：句末标点，或句末标点后面紧跟的引号、括号
bool EndsAt(int i) {
    for (int k = i; k >= 0 && i - k < 4; --k) {
        if (IsEnd(Cp(k))) return true;
        if (!IsCloser(Cp(k))) return false;
    }
    return false;
}
std::string Text(int a, int b) {
    std::string out;
    for (int i = a; i < b && i < int(s_boxes.size()); ++i) {
        const auto& g = s_boxes[i];
        if (g.item < s_items.size() && g.begin + g.len <= s_items[g.item].size()) out.append(s_items[g.item], g.begin, g.len);
    }
    return out;
}
void WordAt(int i, int& a, int& b) {
    a = i; b = i + 1;
    if (!IsWordChar(Cp(i))) return;
    while (a > 0 && !s_para[a] && s_boxes[a - 1].item == s_boxes[a].item && IsWordChar(Cp(a - 1))) --a;
    while (b < int(s_boxes.size()) && !s_para[b] && s_boxes[b].item == s_boxes[b - 1].item && IsWordChar(Cp(b))) ++b;
}
void SentenceAt(int i, int& a, int& b) {
    const int n = int(s_boxes.size());
    a = i;
    while (a > 0 && !s_para[a] && !EndsAt(a - 1)) --a;
    while (a < i && (IsPunct(Cp(a)) && !IsWordChar(Cp(a)))) ++a;    // 不从标点、空格开始
    b = i + 1;
    while (b < n && !s_para[b] && !EndsAt(b - 1)) ++b;
    while (b < n && !s_para[b] && IsCloser(Cp(b))) ++b;
}
bool WordKind() {
    return s.mode == Mode::kWord || (s.mode == Mode::kFree && s.b - s.a <= 4);
}

// ---------- 位图 ----------
lv_obj_t* RasterObject() {
    auto* img = Book_State().reader.text_raster_image;
    return img && lv_obj_is_valid(img) ? img : nullptr;
}
reader::RasterImage* Raster() {
    auto* img = RasterObject();
    if (!img) return nullptr;
    auto* raster = static_cast<reader::RasterImage*>(lv_obj_get_user_data(img));
    if (!raster || raster->empty() || raster->dsc.header.cf != LV_COLOR_FORMAT_L8) return nullptr;
    return raster;
}
void InvertRange(reader::RasterImage& r, int a, int b, lv_area_t& dirty) {
    const int stride = r.dsc.header.stride ? r.dsc.header.stride : r.width;
    for (int i = std::max(0, a); i < b && i < int(s_boxes.size()); ++i) {
        const auto& g = s_boxes[i];
        const int x0 = std::max<int>(0, g.x0), x1 = std::min<int>(r.width, g.x1);
        const int y0 = std::max<int>(0, g.y0 - 1), y1 = std::min<int>(r.height, g.y1);
        if (x0 >= x1 || y0 >= y1) continue;
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = r.pixels.data() + size_t(y) * stride;
            for (int x = x0; x < x1; ++x) p[x] = uint8_t(255 - p[x]);
        }
        dirty.x1 = std::min<int32_t>(dirty.x1, x0); dirty.y1 = std::min<int32_t>(dirty.y1, y0);
        dirty.x2 = std::max<int32_t>(dirty.x2, x1 - 1); dirty.y2 = std::max<int32_t>(dirty.y2, y1 - 1);
    }
}
// 把位图里的反显改成现在的选中：只翻转进出选区的对称差集，避免拖拉时整块多行重绘
void Repaint() {
    auto* raster = Raster();
    auto* img = RasterObject();
    if (!raster || !img) { s.inverted = false; return; }
    lv_area_t dirty{INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    if (!s.inverted) {
        if (s.on) InvertRange(*raster, s.a, s.b, dirty);
    } else if (!s.on) {
        InvertRange(*raster, s.inv_a, s.inv_b, dirty);
    } else if (s.b <= s.inv_a || s.inv_b <= s.a) {
        InvertRange(*raster, s.inv_a, s.inv_b, dirty);
        InvertRange(*raster, s.a, s.b, dirty);
    } else {
        if (s.inv_a != s.a) InvertRange(*raster, std::min(s.inv_a, s.a), std::max(s.inv_a, s.a), dirty);
        if (s.inv_b != s.b) InvertRange(*raster, std::min(s.inv_b, s.b), std::max(s.inv_b, s.b), dirty);
    }
    s.inverted = s.on;
    if (s.on) { s.inv_a = s.a; s.inv_b = s.b; }
    if (dirty.x1 > dirty.x2) return;
    lv_image_cache_drop(&raster->dsc);
    lv_area_t at;
    lv_obj_get_coords(img, &at);
    lv_area_t area{at.x1 + dirty.x1, at.y1 + dirty.y1, at.x1 + dirty.x2, at.y1 + dirty.y2};
    lv_obj_invalidate_area(img, &area);
}

// 屏幕上的点 → 第几个字。nearest：拖动时总取最近的字；长按时离字太远返回 -1
int GlyphAt(lv_point_t pt, bool nearest) {
    auto* img = RasterObject();
    if (!img || s_boxes.empty()) return -1;
    lv_area_t at;
    lv_obj_get_coords(img, &at);
    const int x = pt.x - at.x1, y = pt.y - at.y1;
    int best = -1, best_dx = 0, best_dy = 0;
    long best_d = LONG_MAX;
    for (int i = 0; i < int(s_boxes.size()); ++i) {
        const auto& g = s_boxes[i];
        const int dy = y < g.y0 ? g.y0 - y : y >= g.y1 ? y - g.y1 + 1 : 0;
        const int dx = x < g.x0 ? g.x0 - x : x >= g.x1 ? x - g.x1 + 1 : 0;
        const long d = long(dy) * 4096 + dx;   // 先找行，再找行里的字
        if (d < best_d) { best_d = d; best = i; best_dx = dx; best_dy = dy; }
    }
    if (!nearest && best >= 0) {
        const auto& g = s_boxes[best];
        if (best_dy > (g.y1 - g.y0) / 2 || best_dx > kLongPressReachX) return -1;
    }
    return best;
}

// ---------- 把手 ----------
void HandleEvent(lv_event_t* e);
lv_obj_t* MakeHandle(int which) {
    auto* scr = Book_State().reader.read_scr;
    auto* h = lv_obj_create(scr);
    lv_obj_remove_style_all(h);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(h, kTouchUiPagedVerticalDrag);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    EpdSetContentRole(h, EpdContentRole::GrayPanel);
    auto* bar = lv_obj_create(h);
    lv_obj_remove_style_all(bar);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    auto* dot = lv_obj_create(h);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, kHandleDot, kHandleDot);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    const auto data = reinterpret_cast<void*>(intptr_t(which));
    for (auto code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(h, HandleEvent, code, data);
    lv_obj_add_event_cb(h, [](lv_event_t* ev) {
        const int i = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(ev)));
        if (s_handle[i] == lv_event_get_target(ev)) s_handle[i] = nullptr;
    }, LV_EVENT_DELETE, data);
    return h;
}
// 开头的把手：竖线贴着第一个字的左边，圆点在上；结尾的：贴着最后一个字的右边，圆点在下
void PlaceHandles() {
    auto* img = RasterObject();
    if (!img || !s.on || s.quoted || s.drag == Drag::kGrow) {
        for (auto*& h : s_handle) if (h && lv_obj_is_valid(h)) lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(img, &at);
    for (int which = 0; which < 2; ++which) {
        const bool created = !s_handle[which] || !lv_obj_is_valid(s_handle[which]);
        if (created) s_handle[which] = MakeHandle(which);
        auto* h = s_handle[which];
        const bool was_hidden = created || lv_obj_has_flag(h, LV_OBJ_FLAG_HIDDEN);
        const auto& g = s_boxes[which == 0 ? s.a : s.b - 1];
        const int line_h = g.y1 - g.y0 + 1;
        const int bar_x = at.x1 + (which == 0 ? g.x0 : g.x1) - kHandleBar / 2;
        const int top = at.y1 + g.y0 - 1;
        const int target_w = kHandleW, target_h = line_h + kHandleReach;
        const int target_x = bar_x + kHandleBar / 2 - kHandleW / 2;
        const int target_y = which == 0 ? top - kHandleReach : top;
        if (was_hidden || lv_obj_get_width(h) != target_w || lv_obj_get_height(h) != target_h) {
            lv_obj_set_size(h, target_w, target_h);
            auto* bar = lv_obj_get_child(h, 0);
            auto* dot = lv_obj_get_child(h, 1);
            lv_obj_set_size(bar, kHandleBar, line_h);
            lv_obj_set_pos(bar, kHandleW / 2 - kHandleBar / 2, which == 0 ? kHandleReach : 0);
            lv_obj_set_pos(dot, kHandleW / 2 - kHandleDot / 2,
                           which == 0 ? kHandleReach - kHandleDot + 2 : line_h - 2);
        }
        if (was_hidden || lv_obj_get_x(h) != target_x || lv_obj_get_y(h) != target_y) {
            lv_obj_set_pos(h, target_x, target_y);
        }
        if (was_hidden) {
            lv_obj_clear_flag(h, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(h);
        }
    }
}

// ---------- 卡片 ----------
void BuildCard();
void DeleteCard() {
    if (s_card && lv_obj_is_valid(s_card)) lv_obj_delete(s_card);
    s_card = nullptr;
}
void ShowChrome() {
    PlaceHandles();
    if (s.on && !s.quoted && s.drag == Drag::kNone) BuildCard();
    else DeleteCard();
}
void HideChrome() {
    DeleteCard();
    for (auto*& h : s_handle) if (h && lv_obj_is_valid(h)) lv_obj_add_flag(h, LV_OBJ_FLAG_HIDDEN);
}

// 书里提问用的那一个能用：官方云总能用；自建要有一个配好的助理
bool AgentReady() { return chat_cloud::BookProviderReady(); }

// 预设问题：固定句式，卡片一出来就有，不用联网
std::vector<std::string> Presets() {
    const std::string q = Text(s.a, s.b);
    if (WordKind()) return {
        std::string(T("「", "What does \"")) + q + T("」在这里是什么意思？", "\" mean here?"),
        T("没看懂这句，解释一下", "Explain this sentence"),
    };
    return {T("没看懂，解释一下这句", "Explain this sentence"), T("这句在书里起什么作用？", "Why is this here?"),
            T("讲讲这句的背景", "What's the background?")};
}

void Ask(const std::string* question);
void CardClicked(lv_event_t* e) {
    const auto action = Action(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    HapticPulseIfEnabled();
    switch (action) {
        case kAsk0: case kAsk1: case kAsk2: {
            const auto presets = Presets();
            if (size_t(action) < presets.size()) Ask(&presets[action]);
            return;
        }
        case kLook: {
            if (!WordKind()) return;
            const std::string q = std::string(T("「", "How do you say \"")) + Text(s.a, s.b) +
                                  T("」怎么读，是什么意思？", "\" and what does it mean?");
            Ask(&q);
            return;
        }
        case kSentence:
            if (s.mode == Mode::kSentence) { s.a = s.anchor_a; s.b = s.anchor_b; s.mode = Mode::kWord; }
            else { SentenceAt(s.at, s.a, s.b); s.mode = Mode::kSentence; }
            Repaint();
            ShowChrome();
            return;
        case kAskOwn:
            Ask(nullptr);
            return;
        case kConnect:
            ReaderSelectClear();
            ScreenNavigateTo(SettingsScreen::CreateAssistant);
            return;
    }
}

lv_obj_t* CardButton(lv_obj_t* parent, Action action) {
    auto* b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, CardClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(intptr_t(action)));
    return b;
}
lv_obj_t* CardIcon(lv_obj_t* parent, ValetIcon icon, lv_color_t color) {
    auto* img = lv_image_create(parent);
    lv_image_set_src(img, &valet_icons[icon]);
    lv_obj_set_style_image_recolor(img, color, 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}
lv_obj_t* CardLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void PlaceCard() {
    auto* img = RasterObject();
    if (!s_card || !img) return;
    lv_obj_update_layout(s_card);
    const int h = lv_obj_get_height(s_card);
    lv_area_t at;
    lv_obj_get_coords(img, &at);
    const int sel_top = at.y1 + s_boxes[s.a].y0, sel_bottom = at.y1 + s_boxes[s.b - 1].y1;
    const int screen_h = lv_obj_get_height(Book_State().reader.read_scr);
    int y = sel_top - kCardGap - h;
    if (y < kScreenMargin) y = sel_bottom + kCardGap;   // 上面放不下：放在下面
    y = std::max(kScreenMargin, std::min(y, screen_h - kScreenMargin - h));
    lv_obj_set_pos(s_card, kCardSide, y);
}

void BuildCard() {
    DeleteCard();
    auto* scr = Book_State().reader.read_scr;
    if (!scr || !lv_obj_is_valid(scr)) return;
    const lv_font_t* font = fontpack_lv_font_get(30, 4);
    if (!font) font = fontpack_lv_font_ui();
    const lv_font_t* small = fontpack_lv_font_get(25, 2);
    if (!small) small = font;
    const lv_color_t white = lv_color_white(), black = lv_color_black();
    const lv_color_t rule = lv_color_hex(0x8a8a88);
    const bool agent = AgentReady(), word = WordKind();
    const int width = lv_obj_get_width(scr) - 2 * kCardSide;

    s_card = lv_obj_create(scr);
    auto* card = s_card;
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, black, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // 卡片上的空处不漏到书页
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* ev) {
        if (s_card == lv_event_get_target(ev)) s_card = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    EpdSetContentRole(card, EpdContentRole::GrayPanel);

    // 上面：预设问题，一行一条；没连上自己的助理时换成一句说明和去连接
    auto* top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(top, 8, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);
    const auto add_row = [&](const char* text, Action action, ValetIcon icon, bool rule_above) {
        auto* row = CardButton(top, action);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, kRowMinH, 0);
        lv_obj_set_style_pad_hor(row, kRowPadX, 0);
        lv_obj_set_style_pad_ver(row, 12, 0);
        lv_obj_set_style_pad_column(row, 17, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (rule_above) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_border_color(row, rule, 0);
        }
        CardIcon(row, icon, white);
        auto* label = CardLabel(row, text, font, white);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(label, 1);
    };
    if (agent) {
        const auto presets = Presets();
        for (size_t i = 0; i < presets.size() && i < 3; ++i)
            add_row(presets[i].c_str(), Action(kAsk0 + i), VALET_ICON_SEL_QUEST, i > 0);
    } else {
        auto* tip = CardLabel(top, T("查字典、问 AI 要连上自己的助理。", "Look-up and Ask AI need your own assistant."), font, white);
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tip, LV_PCT(100));
        lv_obj_set_style_pad_hor(tip, kRowPadX, 0);
        lv_obj_set_style_pad_ver(tip, 12, 0);
        add_row(T("去连接", "Connect"), kConnect, VALET_ICON_SEL_MIC, true);
    }

    // 下面一排：查字典 / 整句 / 问 AI。整句是开关：按下去反白，再按回到原来那个词
    auto* tools = lv_obj_create(card);
    lv_obj_remove_style_all(tools);
    lv_obj_set_size(tools, LV_PCT(100), kToolH);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_side(tools, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(tools, 2, 0);
    lv_obj_set_style_border_color(tools, lv_color_hex(0x8a8a88), 0);
    lv_obj_clear_flag(tools, LV_OBJ_FLAG_CLICKABLE);
    const auto add_tool = [&](const char* text, Action action, ValetIcon icon, bool enabled, bool on) {
        auto* b = CardButton(tools, action);
        lv_obj_set_height(b, LV_PCT(100));
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 8, 0);
        const lv_color_t color = on ? black : white;
        if (on) {
            lv_obj_set_style_bg_color(b, white, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        }
        auto* i = CardIcon(b, icon, color);
        auto* l = CardLabel(b, text, small, color);
        if (!enabled) {
            lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
            lv_obj_set_style_opa(i, LV_OPA_40, 0);
            lv_obj_set_style_opa(l, LV_OPA_40, 0);
        }
    };
    add_tool(T("查字典", "Dictionary"), kLook, VALET_ICON_SEL_BOOK, agent && word, false);
    add_tool(T("整句", "Sentence"), kSentence, VALET_ICON_SEL_SENTENCE, true, s.mode == Mode::kSentence);
    add_tool(T("问 AI", "Ask AI"), kAskOwn, VALET_ICON_SEL_MIC, agent, false);
    PlaceCard();
}

// 拿去问：选中的字留着反显，悬窗在书页下方打开。question 为空 = 「问 AI」，停在悬窗等按住说话
void Ask(const std::string* question) {
    if (!s.on) return;
    int sa = 0, sb = 0;
    SentenceAt(s.at, sa, sb);
    valet::ReadingQuote quote;
    quote.text = Text(s.a, s.b);
    quote.sentence = Text(sa, sb);
    quote.word = WordKind();
    ESP_LOGI(TAG, "ask about %u bytes (%s)%s", unsigned(quote.text.size()), quote.word ? "word" : "sentence",
             question ? ", preset question" : ", hold to talk");
    s.quoted = true;
    EpdSetUiScrolling(false);
    HideChrome();
    valet::SetReadingQuote(quote);
    lv_coord_t below = 0;
    if (auto* img = RasterObject()) {
        lv_area_t at;
        lv_obj_get_coords(img, &at);
        below = lv_coord_t(at.y1 + s_boxes[s.b - 1].y1 + 2);
    }
    if (!OpenReaderAskPanel(false, below, question ? *question : std::string())) { valet::ClearReadingQuote(); ReaderSelectClear(); }
}

void PanelClosed() {
    if (s.quoted) ReaderSelectClear();
}

// ---------- 触控 ----------
bool CanSelect() {
    auto& st = Book_State();
    return st.reader.read_scr && lv_obj_is_valid(st.reader.read_scr) && st.reader.read_scr == lv_screen_active() &&
           st.reader.read_chrome == BookUiState::ReadChrome::kReading && st.reader.session && !s_boxes.empty();
}

void SetVerticalDragLock(bool on) {
    auto* content = Book_State().reader.content;
    if (!content || !lv_obj_is_valid(content)) return;
    if (on) lv_obj_add_flag(content, kTouchUiPagedVerticalDrag);
    else lv_obj_clear_flag(content, kTouchUiPagedVerticalDrag);
}

bool SetRange(int a, int b, Mode mode) {
    if (a == s.a && b == s.b) return false;
    s.a = a; s.b = b; s.mode = mode;
    Repaint();
    return true;
}

void OnContentLongPressed(lv_event_t* e) {
    if (!CanSelect()) return;
    lv_point_t pt{};
    if (!ReadEventPoint(e, &pt)) return;
    const int i = GlyphAt(pt, false);
    if (i < 0 || IsPunct(Cp(i))) return;
    if (AssistantScreen::PanelOpen()) AssistantScreen::ClosePanel();   // 换一处问：先收起上一次的悬窗
    if (s.on) ReaderSelectClear();
    int a = 0, b = 0;
    WordAt(i, a, b);
    s.on = true; s.quoted = false; s.at = i;
    s.anchor_a = a; s.anchor_b = b; s.a = a; s.b = b; s.mode = Mode::kWord;
    s.drag = Drag::kGrow;
    s_eat_click = true;
    SetVerticalDragLock(true);
    EpdSetUiScrolling(true);
    ReaderAutoTurnManualInput();
    HapticPulseIfEnabled();
    Repaint();
    HideChrome();   // 手指还按着：只反显，卡片等松手
    ESP_LOGI(TAG, "long press: %d glyphs selected", b - a);
}

void OnContentPressing(lv_event_t* e) {
    if (s.drag != Drag::kGrow || !s.on) return;
    lv_point_t pt{};
    if (!ReadEventPoint(e, &pt)) return;
    const int i = GlyphAt(pt, true);
    if (i < 0) return;
    const int a = std::min(s.anchor_a, i), b = std::max(s.anchor_b, i + 1);
    SetRange(a, b, a == s.anchor_a && b == s.anchor_b ? Mode::kWord : Mode::kFree);
}

void OnContentReleased(lv_event_t*) {
    if (s.drag != Drag::kGrow) return;
    s.drag = Drag::kNone;
    if (s.on) ShowChrome();
}

void HandleEvent(lv_event_t* e) {
    const int which = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    const auto code = lv_event_get_code(e);
    if (!s.on || s.quoted) return;
    lv_point_t pt{};
    if (code == LV_EVENT_PRESSED) {
        if (!ReadEventPoint(e, &pt)) return;
        int pick = which;
        auto* img = RasterObject();
        if (img && s.a >= 0 && s.b > s.a && s.b <= int(s_boxes.size())) {
            lv_area_t at;
            lv_obj_get_coords(img, &at);
            const auto& g0 = s_boxes[s.a];
            const auto& g1 = s_boxes[s.b - 1];
            const long dx0 = pt.x - (at.x1 + g0.x0), dy0 = pt.y - (at.y1 + g0.y0 - kHandleDot / 2);
            const long dx1 = pt.x - (at.x1 + g1.x1), dy1 = pt.y - (at.y1 + g1.y1 + kHandleDot / 2);
            pick = (dx0 * dx0 + dy0 * dy0 <= dx1 * dx1 + dy1 * dy1) ? 0 : 1;
            const auto& g = pick == 0 ? g0 : g1;
            s.drag_dy = at.y1 + (g.y0 + g.y1) / 2 - pt.y;   // 手指按在圆点附近，换算回那一行
        }
        s.drag = pick == 0 ? Drag::kStart : Drag::kEnd;
        EpdSetUiScrolling(true);
        DeleteCard();
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (s.drag != Drag::kStart && s.drag != Drag::kEnd) return;
        if (!ReadEventPoint(e, &pt)) return;
        pt.y += s.drag_dy;
        const int i = GlyphAt(pt, true);
        if (i < 0) return;
        const bool changed = s.drag == Drag::kStart ? SetRange(std::min(i, s.b - 1), s.b, Mode::kFree)
                                                    : SetRange(s.a, std::max(i + 1, s.a + 1), Mode::kFree);
        if (changed) PlaceHandles();
        return;
    }
    // 松手：卡片回来
    if (s.drag == Drag::kStart || s.drag == Drag::kEnd) {
        s.drag = Drag::kNone;
        ShowChrome();
    }
}
}  // namespace

void ReaderSelectSetPage(std::vector<reader::GlyphBox> glyphs, const reader::Page* page) {
    // 位图已经换成新的一页：原来的反显随旧位图没了，不用翻回来
    const bool was_quoted = s.quoted;
    s = {};
    s_eat_click = false;
    SetVerticalDragLock(false);
    EpdSetUiScrolling(false);
    HideChrome();
    if (was_quoted) valet::ClearReadingQuote();   // 翻页了：再问就是问新的这一页
    s_boxes = std::move(glyphs);
    s_items.clear();
    s_para.clear();
    if (!page) { s_boxes.clear(); return; }
    s_items.reserve(page->items.size());
    for (const auto& item : page->items) s_items.push_back(item.kind == reader::ContentKind::kText ? item.text : std::string());
    s_para.assign(s_boxes.size(), 0);
    for (size_t i = 0; i < s_boxes.size(); ++i) {
        const auto item = s_boxes[i].item;
        if (i == 0) { s_para[i] = 1; continue; }
        if (item == s_boxes[i - 1].item || item >= page->items.size()) continue;
        const auto& it = page->items[item];
        const auto prev = s_boxes[i - 1].item;
        const bool after_title = prev < page->items.size() && page->items[prev].chapter_title;
        s_para[i] = it.para_indent || it.para_gap_before || it.chapter_title || after_title;
    }
    AssistantScreen::SetPanelClosedHook(PanelClosed);
}

void ReaderSelectClear() {
    const bool had = s.on || s.inverted;
    s.on = false;
    s.quoted = false;
    s.drag = Drag::kNone;
    SetVerticalDragLock(false);
    HideChrome();
    if (had) {
        EpdSetUiScrolling(true);
        EpdAfterNextSubmit([]() { EpdSetUiScrolling(false); });
        Repaint();
    } else {
        EpdSetUiScrolling(false);
    }
}

bool ReaderSelectActive() { return s.on; }

bool ReaderSelectConsumeClick() {
    if (s_eat_click) { s_eat_click = false; return true; }
    if (s.on && !s.quoted) { ReaderSelectClear(); return true; }   // 点正文：收起选中
    return false;
}

void ReaderSelectAttach(lv_obj_t* content) {
    if (!content) return;
    ReaderSelectDetach(content);
    lv_obj_add_event_cb(content, OnContentLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(content, OnContentPressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(content, OnContentReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(content, OnContentReleased, LV_EVENT_PRESS_LOST, nullptr);
}

void ReaderSelectDetach(lv_obj_t* content) {
    if (!content) return;
    lv_obj_remove_event_cb(content, OnContentLongPressed);
    lv_obj_remove_event_cb(content, OnContentPressing);
    while (lv_obj_remove_event_cb(content, OnContentReleased)) {}
}
