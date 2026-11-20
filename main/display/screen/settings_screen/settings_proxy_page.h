#pragma once

#include "lvgl.h"

#include <string>

// 设置 → 网络 → 代理: one proxy address, a switch for Wi-Fi and one for 4G,
// HTTP or SOCKS5, an optional login, and a test.
void SettingsProxyPage_Build(lv_obj_t* page);
void SettingsProxyPage_Reset();
/** @brief Close the text editor if it is open. Returns true when it was. */
bool SettingsProxyPage_CloseEditor();
/** @brief One line for the network page's 「代理」 row. */
std::string SettingsProxyPage_Summary();
