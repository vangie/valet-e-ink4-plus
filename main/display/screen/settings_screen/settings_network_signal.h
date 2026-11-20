#pragma once

#include <lvgl.h>

/** @brief 创建实时网络信号栏，控件删除时停止采样；同一时间仅支持一个可见信号栏。 */
void SettingsNetworkSignal_Build(lv_obj_t* parent);

// Valet network page: the connection's state as a card for the top of the page
// ("4G · 信号好", "Wi-Fi · 省电待命" …), kept current while the page is open.
// Use instead of SettingsNetworkSignal_Build, not together with it.
void SettingsNetworkSignal_BuildCard(lv_obj_t* parent);
/** @brief One small line with the SIM's number (ICCID), for under the SIM choice. Call after BuildCard. */
void SettingsNetworkSignal_BuildCardNumber(lv_obj_t* parent);
