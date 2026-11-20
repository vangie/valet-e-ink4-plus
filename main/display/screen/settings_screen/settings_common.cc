#include "settings_common.h"

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_text.h"

#include <cstring>

void SettingsStyleBrightnessButton(lv_obj_t* button) {
    lv_obj_set_size(button, kSettingsBrightnessButtonW, kSettingsBrightnessButtonH);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, kSettingsOptionRadius, 0);
    lv_obj_set_style_border_width(button, kSettingsBorderW, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    auto* label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
}

void SettingsStyleSelectable(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    lv_obj_set_style_bg_color(btn, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, kSettingsBorderW, 0);
    lv_obj_set_style_radius(btn, kSettingsOptionRadius, 0);
    if (lbl != nullptr) {
        lv_obj_set_style_text_color(lbl, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

lv_obj_t* SettingsCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t cb,
                                         intptr_t user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, kSettingsOptionH);
    lv_obj_set_style_pad_all(btn, kSettingsOptionPad, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(user_data));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, ScreenFitButtonLabelOnSize, LV_EVENT_SIZE_CHANGED, lbl);

    SettingsStyleSelectable(btn, lbl, false);
    return btn;
}

// ── Valet controls
namespace {
const lv_font_t* ValetSmall() {
    const auto* font = fontpack_lv_font_get(25, 2);
    return font ? font : fontpack_lv_font_ui();
}
const lv_font_t* ValetStrong() {
    const auto* font = fontpack_lv_font_get(30, 4);
    return font ? font : fontpack_lv_font_ui();
}
constexpr int kSegH = 76, kSegFrame = 3;
}

lv_obj_t* SettingsValetLabel(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ValetSmall(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_margin_bottom(label, 16, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* SettingsValetNote(lv_obj_t* parent, const char* text) {
    auto* note = lv_label_create(parent);
    lv_label_set_text(note, text);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(note, ValetSmall(), 0);
    lv_obj_set_style_text_color(note, lv_color_black(), 0);
    lv_obj_set_style_text_line_space(note, 6, 0);
    lv_obj_set_style_margin_top(note, 20, 0);
    lv_obj_clear_flag(note, LV_OBJ_FLAG_CLICKABLE);
    return note;
}

lv_obj_t* SettingsValetSegments(lv_obj_t* parent, const char* const* names, int count, lv_event_cb_t cb,
                                const intptr_t* values, int columns) {
    if (columns <= 0 || columns > count) columns = count;
    const int rows = (count + columns - 1) / columns;
    auto* frame = lv_obj_create(parent);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, lv_pct(100), rows * kSegH + 2 * kSegFrame);
    lv_obj_set_style_border_width(frame, kSegFrame, 0);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(frame);
    const int inner = lv_obj_get_content_width(frame);
    for (int i = 0; i < count; ++i) {
        const int column = i % columns, row = i / columns;
        const int x0 = inner * column / columns, x1 = inner * (column + 1) / columns;
        auto* cell = lv_obj_create(frame);
        lv_obj_remove_style_all(cell);
        lv_obj_set_pos(cell, x0, row * kSegH);
        lv_obj_set_size(cell, x1 - x0, kSegH);
        // Hairlines between cells; the frame draws the outside.
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_side(cell, static_cast<lv_border_side_t>(
            (column ? LV_BORDER_SIDE_LEFT : LV_BORDER_SIDE_NONE) | (row ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_NONE)), 0);
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cell);
        lv_obj_add_event_cb(cell, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(values ? values[i] : static_cast<intptr_t>(i)));
        auto* label = lv_label_create(cell);
        lv_label_set_text(label, names[i]);
        lv_obj_set_style_text_font(label, fontpack_lv_font_ui(), 0);
        lv_obj_set_style_text_color(label, lv_color_black(), 0);
        lv_obj_center(label);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return frame;
}

void SettingsValetSelect(lv_obj_t* segments, int selected) {
    if (!segments) return;
    const int count = static_cast<int>(lv_obj_get_child_count(segments));
    for (int i = 0; i < count; ++i) {
        auto* cell = lv_obj_get_child(segments, i);
        auto* label = lv_obj_get_child(cell, 0);
        const bool on = i == selected;
        // Skip cells already in the right state: restyling redraws, and a redraw is a panel refresh.
        if (lv_color_eq(lv_obj_get_style_bg_color(cell, LV_PART_MAIN), on ? lv_color_black() : lv_color_white()) &&
            lv_obj_get_style_text_font(label, LV_PART_MAIN) == (on ? ValetStrong() : fontpack_lv_font_ui())) continue;
        lv_obj_set_style_bg_color(cell, on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(label, on ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_font(label, on ? ValetStrong() : fontpack_lv_font_ui(), 0);
        lv_obj_center(label);
    }
}

void SettingsValetGap(lv_obj_t* parent, int height) {
    auto* gap = lv_obj_create(parent);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, height);
    lv_obj_clear_flag(gap, LV_OBJ_FLAG_CLICKABLE);
}

namespace {
// Check mark or cross, drawn: the UI font pack has no symbol glyphs.
void DrawCardMark(lv_event_t* e) {
    const intptr_t kind = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const bool ok = kind != 0;
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = ok ? lv_color_white() : lv_color_black();
    line.width = 4;
    line.opa = LV_OPA_COVER;
    line.round_start = line.round_end = 1;
    const int w = a.x2 - a.x1;
    auto seg = [&](int x1, int y1, int x2, int y2) {
        line.p1 = {static_cast<lv_value_precise_t>(a.x1 + x1 * w / 24), static_cast<lv_value_precise_t>(a.y1 + y1 * w / 24)};
        line.p2 = {static_cast<lv_value_precise_t>(a.x1 + x2 * w / 24), static_cast<lv_value_precise_t>(a.y1 + y2 * w / 24)};
        lv_draw_line(lv_event_get_layer(e), &line);
    };
    if (kind == 2) { seg(7, 3, 17, 3); seg(17, 3, 17, 21); seg(17, 21, 7, 21); seg(7, 21, 7, 3); seg(10, 18, 14, 18); }
    else if (ok) { seg(5, 13, 10, 17); seg(10, 17, 19, 8); }
    else { seg(6, 6, 18, 18); seg(18, 6, 6, 18); }
}
// LVGL borders cannot be dashed; draw the outline as four dashed lines.
void DrawDashedFrame(lv_event_t* e) {
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t*>(lv_event_get_target(e)), &a);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = 3;
    line.opa = LV_OPA_COVER;
    line.dash_width = 10;
    line.dash_gap = 8;
    const lv_value_precise_t x1 = a.x1 + 1, y1 = a.y1 + 1, x2 = a.x2 - 1, y2 = a.y2 - 1;
    const lv_point_precise_t corners[5] = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}, {x1, y1}};
    for (int i = 0; i < 4; ++i) {
        line.p1 = corners[i];
        line.p2 = corners[i + 1];
        lv_draw_line(lv_event_get_layer(e), &line);
    }
}
}

lv_obj_t* SettingsValetCard(lv_obj_t* parent, const char* title, const char* detail, bool ok, bool device_icon) {
    auto* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), 138);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (ok) {
        lv_obj_set_style_bg_color(card, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_event_cb(card, DrawDashedFrame, LV_EVENT_DRAW_MAIN, nullptr);
    }
    auto* mark = lv_obj_create(card);
    lv_obj_remove_style_all(mark);
    lv_obj_set_size(mark, 50, 50);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 28, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mark, DrawCardMark, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void*>(static_cast<intptr_t>(!ok ? 0 : device_icon ? 2 : 1)));
    const lv_color_t ink = ok ? lv_color_white() : lv_color_black();
    auto* name = lv_label_create(card);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, ValetStrong(), 0);
    lv_obj_set_style_text_color(name, ink, 0);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_pad_right(name, 130, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 106, -22);
    auto* sub = lv_label_create(card);
    lv_label_set_text(sub, detail);
    lv_obj_set_style_text_font(sub, ValetSmall(), 0);
    lv_obj_set_style_text_color(sub, ink, 0);
    lv_obj_set_width(sub, lv_pct(100));
    lv_obj_set_style_pad_right(sub, 130, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 106, 24);
    return card;
}

void SettingsValetCardSet(lv_obj_t* card, const char* title, const char* detail) {
    if (!card || lv_obj_get_child_count(card) < 3) return;
    // Children in the order SettingsValetCard creates them: mark, title, detail.
    const char* const texts[2] = {title, detail};
    for (int i = 0; i < 2; ++i) {
        auto* label = lv_obj_get_child(card, i + 1);
        if (std::strcmp(lv_label_get_text(label), texts[i]) != 0) lv_label_set_text(label, texts[i]);
    }
}

lv_obj_t* SettingsValetButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, intptr_t data, bool solid) {
    auto* button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, lv_pct(100), 78);
    lv_obj_set_style_border_width(button, 3, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_bg_color(button, solid ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(data));
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, solid ? ValetStrong() : fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(label, solid ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

