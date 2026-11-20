#include "settings_common.h"

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_text.h"

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

