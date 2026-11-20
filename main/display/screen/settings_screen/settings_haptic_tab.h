#pragma once

#include "lvgl.h"

/** @brief 构建设置→震动页选项 */
void SettingsHapticTab_Build(lv_obj_t* page);
/** @brief 重置震动页 UI 指针 */
void SettingsHapticTab_Reset();

/** @brief Valet layout: one off / weak / medium / strong control, for use inside another page. */
void SettingsHapticTab_BuildValet(lv_obj_t* page);
