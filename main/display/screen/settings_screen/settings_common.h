#pragma once

#include "lvgl.h"

#include "fontpack_lvgl.h"
#include "ui_scale.h"

#include <cstdint>

// 源值对齐 397 settings_screen；经 UiSx/UiSy 同比例到 470
constexpr lv_coord_t kSettingsBodyPad = UiSx(10); // 设置页内容区内边距
constexpr lv_coord_t kSettingsTabW = UiSx(120); // 侧栏 Tab 宽
constexpr lv_coord_t kSettingsTabH = UiSy(44); // 侧栏 Tab 高
constexpr lv_coord_t kSettingsTabGap = UiSy(8); // Tab 间距
constexpr lv_coord_t kSettingsSplitLineW = UiSx(2); // 分区分割线宽
constexpr lv_coord_t kSettingsContentPad = UiSx(14); // 右侧内容区内边距
constexpr lv_coord_t kSettingsOptionH = UiSy(48); // 可选中选项行高
constexpr lv_coord_t kSettingsOptionGap = UiSy(12); // 选项间距
constexpr lv_coord_t kSettingsBorderW = UiSx(2); // 描边宽度
constexpr lv_coord_t kSettingsOptionRadius = 0; // Valet: square corners, like the segmented choices
constexpr lv_coord_t kSettingsOptionPad = UiSx(8);
constexpr lv_coord_t kSettingsBrightBtnW = UiSx(96);
constexpr lv_coord_t kSettingsInfoRowH = UiSy(56);
constexpr lv_coord_t kSettingsInfoRowGap = UiSy(8);
constexpr lv_coord_t kSettingsInfoRowPadH = UiSx(12);
constexpr lv_coord_t kSettingsInfoRowPadV = UiSy(8);

/** @brief 可选中按钮外观：选中黑底白字 / 未选中白底黑字描边 */
void SettingsStyleSelectable(lv_obj_t* btn, lv_obj_t* lbl, bool selected);

constexpr lv_coord_t kSettingsBrightnessButtonW = UiSx(48);
constexpr lv_coord_t kSettingsBrightnessButtonH = UiSy(38);
constexpr lv_coord_t kSettingsBrightnessSliderGap = UiSx(18);
void SettingsStyleBrightnessButton(lv_obj_t* button);

// ── Valet controls, sized from sim/ (新设计 → 设置): a small field label, a
// segmented choice in one frame, and a small note under it.
/** @brief Small label above a control. */
lv_obj_t* SettingsValetLabel(lv_obj_t* parent, const char* text);
/** @brief Small explanatory text under a control; wraps. */
lv_obj_t* SettingsValetNote(lv_obj_t* parent, const char* text);
/** @brief One frame holding `count` choices; `columns` < `count` wraps into rows. Returns the frame. */
lv_obj_t* SettingsValetSegments(lv_obj_t* parent, const char* const* names, int count, lv_event_cb_t cb,
                                const intptr_t* values = nullptr, int columns = 0);
/** @brief Mark the chosen segment (index within the frame; -1 for none). */
void SettingsValetSelect(lv_obj_t* segments, int selected);
/** @brief Status card: black when all is well, a dashed outline when something needs doing. Returns the card. */
lv_obj_t* SettingsValetCard(lv_obj_t* parent, const char* title, const char* detail, bool ok = true,
                            bool device_icon = false);
/** @brief Change a card's two lines. Unchanged text is not redrawn (a redraw is a panel refresh). */
void SettingsValetCardSet(lv_obj_t* card, const char* title, const char* detail);
/** @brief A place for a status card whose kind (solid / dashed) may change while the page is open. */
lv_obj_t* SettingsValetCardSlot(lv_obj_t* parent);
/** @brief Show this state in the slot: the card is rebuilt only when its kind changes, and
 *         unchanged text is not redrawn. */
void SettingsValetCardShow(lv_obj_t* slot, const char* title, const char* detail, bool ok = true,
                           bool device_icon = false);
/** @brief Page header band: back chevron and title. Returns the title label; its parent is the band. */
lv_obj_t* SettingsValetHeader(lv_obj_t* parent, const char* title, lv_event_cb_t back);
/** @brief A ruled row: strong title, small detail under it. With a callback it is tappable and
 *         shows a chevron. `first` adds the rule above. Returns the detail label; its parent is the row. */
lv_obj_t* SettingsValetRow(lv_obj_t* parent, const char* title, const char* detail,
                           lv_event_cb_t callback = nullptr, intptr_t data = 0, bool first = false);
/** @brief A container that lays its children out two to a line; give each child SettingsValetHalf(). */
lv_obj_t* SettingsValetPairs(lv_obj_t* parent);
/** @brief Size a button for a SettingsValetPairs container. */
void SettingsValetHalf(lv_obj_t* button, int height = 70);
/** @brief Outlined (or solid) button of the design's height; width is the caller's. */
lv_obj_t* SettingsValetButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, intptr_t data = 0, bool solid = false);
/** @brief Vertical space between two fields. */
void SettingsValetGap(lv_obj_t* parent, int height = 38);

/** @brief 创建整行可选中选项 */
lv_obj_t* SettingsCreateSelectableOption(lv_obj_t* parent, const char* title, lv_event_cb_t cb,
                                         intptr_t user_data);
