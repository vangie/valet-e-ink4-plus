#pragma once

#include "lvgl.h"

// 设置 → 设备 → 存储与 U 盘: the SD card's state and room, and the way to the USB disk.
void SettingsStoragePage_Build(lv_obj_t* page);
void SettingsStoragePage_Reset();
/** @brief One line for the device page's row: card in or not. Does not touch the card. */
const char* SettingsStoragePage_Summary();
