#pragma once

#include <lvgl.h>

/** @brief 构建设置中的测试入口页。 */
void SettingsTestTab_Build(lv_obj_t* page);
/** @brief Valet layout: the tests two to a line, then power off / restart. For the 诊断 page. */
void SettingsTestTab_BuildValet(lv_obj_t* page);
