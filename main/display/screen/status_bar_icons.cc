#include "status_bar_icons.h"

#include "screen_common.h"
#include "status_icons.h"
#include "ui_scale.h"

#include <cstdio>
#include <initializer_list>
#include <cstring>

#include <font_awesome.h>
#include "src/misc/lv_area_private.h"

namespace {
// Sizes in panel pixels. Wi-Fi, 4G and the battery are the same height (the icon bitmaps are made for it).
constexpr int kIconH = 27;
constexpr int kProxyGap = 5;          // between the network icon and the shield
// Battery: outline 44 x 26 with a 3 px frame, a 2 px gap, then the level; a small cap on the right.
constexpr int kBattW = 44, kBattH = 26, kBattFrame = 3, kBattGap = 2, kBattRadius = 9;
constexpr int kBattCapGap = 2, kBattCapW = 3, kBattCapH = 10;
constexpr int kBattTotalW = kBattW + kBattCapGap + kBattCapW;
constexpr int kDigitGap = 1;          // between digits, and before the bolt
// The clock's digits sit a little below the bar's middle (their label is nudged down); icons follow them.
constexpr int kIconNudgeY = 5;

const lv_image_dsc_t* Icon(ValetIcon id) { return &valet_icons[id]; }

void DrawIcon(lv_layer_t* layer, const lv_image_dsc_t* img, int x, int y, lv_color_t color) {
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = img;
    dsc.recolor = color;
    dsc.recolor_opa = LV_OPA_COVER;
    const lv_area_t area{x, y, x + int(img->header.w) - 1, y + int(img->header.h) - 1};
    lv_draw_image(layer, &dsc, &area);
}

bool StartsWith(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

const lv_image_dsc_t* NetworkIcon(const char* text) {
    struct { const char* glyph; ValetIcon icon; } const kMap[] = {
        {FONT_AWESOME_WIFI_FAIR, VALET_ICON_SB_WIFI_2}, {FONT_AWESOME_WIFI_WEAK, VALET_ICON_SB_WIFI_1},
        {FONT_AWESOME_WIFI_STANDBY, VALET_ICON_SB_WIFI_0},
        {FONT_AWESOME_WIFI, VALET_ICON_SB_WIFI_3}, {FONT_AWESOME_PLANE, VALET_ICON_SB_PLANE},
        {FONT_AWESOME_SIGNAL_STRONG, VALET_ICON_SB_CELL_4}, {FONT_AWESOME_SIGNAL_GOOD, VALET_ICON_SB_CELL_3},
        {FONT_AWESOME_SIGNAL_FAIR, VALET_ICON_SB_CELL_2}, {FONT_AWESOME_SIGNAL_WEAK, VALET_ICON_SB_CELL_1},
        {FONT_AWESOME_SIGNAL_OFF, VALET_ICON_SB_CELL_0},
    };
    for (const auto& m : kMap)
        if (StartsWith(text, m.glyph)) return Icon(m.icon);
    return nullptr;
}

void DrawNetwork(lv_event_t* e) {
    auto* label = lv_event_get_target_obj(e);
    const char* text = lv_label_get_text(label);
    if (!text || !*text) return;
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(label, &a);
    const int mid = (a.y1 + a.y2 + 1) / 2;
    int x = a.x1;
    if (const auto* net = NetworkIcon(text)) {
        DrawIcon(layer, net, x, mid - int(net->header.h) / 2, lv_color_black());
        x += net->header.w + kProxyGap;
    }
    if (std::strchr(text, kStatusProxyMark)) {
        const auto* shield = Icon(VALET_ICON_SB_PROXY);
        DrawIcon(layer, shield, x, mid - int(shield->header.h) / 2, lv_color_black());
    }
}

void FillRound(lv_layer_t* layer, const lv_area_t& area, int radius, lv_color_t color) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_draw_rect(layer, &dsc, &area);
}

void DrawBattery(lv_event_t* e) {
    auto* label = lv_event_get_target_obj(e);
    const char* text = lv_label_get_text(label);
    if (!text || !*text) return;
    int level = 0;
    std::sscanf(text, "%d", &level);
    level = level < 0 ? 0 : level > 100 ? 100 : level;
    const bool charging = std::strchr(text, '+') != nullptr;
    auto* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(label, &a);
    const int x0 = a.x1, y0 = (a.y1 + a.y2 + 1) / 2 - kBattH / 2;

    // Outline: a black rounded box with the inside cleared to white.
    FillRound(layer, {x0, y0, x0 + kBattW - 1, y0 + kBattH - 1}, kBattRadius, lv_color_black());
    FillRound(layer, {x0 + kBattFrame, y0 + kBattFrame, x0 + kBattW - 1 - kBattFrame, y0 + kBattH - 1 - kBattFrame},
              kBattRadius - kBattFrame, lv_color_white());
    const int cap_x = x0 + kBattW + kBattCapGap, cap_y = y0 + (kBattH - kBattCapH) / 2;
    FillRound(layer, {cap_x, cap_y, cap_x + kBattCapW - 1, cap_y + kBattCapH - 1}, 1, lv_color_black());

    // Level: the whole inner shape is drawn, then cut on the right, so the left corners stay round.
    const int in = kBattFrame + kBattGap;
    const lv_area_t fill{x0 + in, y0 + in, x0 + kBattW - 1 - in, y0 + kBattH - 1 - in};
    const int fill_end = fill.x1 + (lv_area_get_width(&fill) * level + 50) / 100 - 1;
    const auto clip = layer->_clip_area;
    lv_area_t level_clip{fill.x1, fill.y1, fill_end, fill.y2};
    if (fill_end >= fill.x1 && lv_area_intersect(&level_clip, &level_clip, &clip)) {
        layer->_clip_area = level_clip;
        FillRound(layer, fill, kBattRadius - in, lv_color_black());
        layer->_clip_area = clip;
    }

    // Digits (and the bolt) centred in the inside: white where they sit on the level, black elsewhere.
    char digits[4] = {};
    std::snprintf(digits, sizeof(digits), "%d", level);
    int width = 0;
    for (const char* c = digits; *c; ++c) width += Icon(ValetIcon(VALET_ICON_DIGIT_0 + (*c - '0')))->header.w + (c[1] ? kDigitGap : 0);
    const auto* bolt = Icon(VALET_ICON_SB_BOLT);
    if (charging) width += kDigitGap + bolt->header.w;
    const int inner_x1 = x0 + kBattFrame, inner_w = kBattW - 2 * kBattFrame;
    int x = inner_x1 + (inner_w - width + 1) / 2;
    const int mid = y0 + kBattH / 2;
    for (bool on_level : {true, false}) {
        lv_area_t part = clip;
        if (on_level) part.x2 = LV_MIN(part.x2, fill_end);
        else part.x1 = LV_MAX(part.x1, fill_end + 1);
        if (part.x1 > part.x2) continue;
        layer->_clip_area = part;
        const lv_color_t color = on_level ? lv_color_white() : lv_color_black();
        int cx = x;
        for (const char* c = digits; *c; ++c) {
            const auto* d = Icon(ValetIcon(VALET_ICON_DIGIT_0 + (*c - '0')));
            DrawIcon(layer, d, cx, mid - int(d->header.h) / 2, color);
            cx += d->header.w + kDigitGap;
        }
        if (charging) DrawIcon(layer, bolt, cx, mid - int(bolt->header.h) / 2, color);
    }
    layer->_clip_area = clip;
}

void DrawMute(lv_event_t* e) {
    auto* label = lv_event_get_target_obj(e);
    const char* text = lv_label_get_text(label);
    if (!text || !*text) return;
    lv_area_t a;
    lv_obj_get_coords(label, &a);
    const auto* icon = Icon(VALET_ICON_SB_MUTE);
    DrawIcon(lv_event_get_layer(e), icon, a.x1, (a.y1 + a.y2 + 1) / 2 - int(icon->header.h) / 2, lv_color_black());
}

// The label keeps its text (the adapter compares and sets it); only the glyphs are not drawn.
void MakeIconView(lv_obj_t* label, int width, int height, lv_event_cb_t draw) {
    if (!label) return;
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_left(label, 0, 0);
    lv_obj_set_style_translate_y(label, kIconNudgeY, 0);
    lv_obj_add_event_cb(label, draw, LV_EVENT_DRAW_MAIN_END, nullptr);
}
} // namespace

void StatusBarIconsAttach(const EpdStatusBar& bar) {
    // The hit area's size is set but not laid out yet: read the style, not the coordinates.
    const int net_w = lv_obj_get_style_width(lv_obj_get_parent(bar.network_label), LV_PART_MAIN);
    MakeIconView(bar.network_label, net_w, kIconH + 8, DrawNetwork);
    lv_obj_align(bar.network_label, LV_ALIGN_LEFT_MID, 0, 0);
    MakeIconView(bar.battery_label, kBattTotalW, kBattH, DrawBattery);
    lv_obj_set_style_translate_y(bar.battery_label, kIconNudgeY + 2, 0);   // 电池框比网络图标矮，单独对准数字
    MakeIconView(bar.mute_label, Icon(VALET_ICON_SB_MUTE)->header.w, kIconH, DrawMute);
    lv_obj_set_style_margin_right(bar.mute_label, 6, 0);
    lv_obj_add_flag(bar.mute_label, LV_OBJ_FLAG_HIDDEN);
    if (bar.battery_pct_label) lv_obj_add_flag(bar.battery_pct_label, LV_OBJ_FLAG_HIDDEN);
}

void StatusBarBatteryText(char* out, size_t size, int level, bool charging) {
    std::snprintf(out, size, "%d%s", level, charging ? "+" : "");
}

void StatusBarSetMuted(lv_obj_t* mute_label, bool muted) {
    if (!mute_label) return;
    lv_label_set_text(mute_label, muted ? "M" : "");
    if (muted) lv_obj_remove_flag(mute_label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(mute_label, LV_OBJ_FLAG_HIDDEN);
}
