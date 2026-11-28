#pragma once
// 书架上的「搜书」与「书库」：
// - 「搜书」：书架一级入口，直接调起搜索键盘，跨微信读书与所有已订阅 OPDS 书库搜书、下载阅读。
// - 「书库」：配置和管理不同书库（微信读书账号扫码登录/同步、添加/移除/认证 OPDS 书库订阅，也可点进书库浏览）。

#include <lvgl.h>

constexpr const char* kScreenOpds = "book_opds"; // 书库配置与在线搜书

/** @brief 书架标题行是否显示「搜书」与「书库」入口 */
bool OpdsHasLibraries();
/** @brief 创建书库/搜书页 */
lv_obj_t* CreateOpdsScreen();
/** @brief 书架标题行「搜书」一级入口（进页即弹搜索键盘） */
void OnShelfSearchClicked(lv_event_t* e);
/** @brief 书架标题行「书库」配置入口（管理不同书库源） */
void OnShelfLibraryClicked(lv_event_t* e);
