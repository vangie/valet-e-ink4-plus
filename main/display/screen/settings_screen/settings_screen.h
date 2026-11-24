#pragma once

#include "lvgl.h"

// 设置主壳：网络 / 主题 / 语言 / 震动 / 前光 / 对话 / 蓝牙 / 测试 / 关于
class SettingsScreen {
public:
    /** @brief 创建设置总览（六组入口） */
    static lv_obj_t* Create();
    /** @brief 创建设置→网络子页入口 */
    static lv_obj_t* CreateNetwork();
    /** @brief 设置 → 网络 → 代理（下拉栏里长按「代理」） */
    static lv_obj_t* CreateProxy();
    static lv_obj_t* CreateAssistant();
    /** @brief 创建设置→翻页器子页入口 */
    static lv_obj_t* CreatePager();
    /** @brief 创建设置→蓝牙子页入口 */
    static lv_obj_t* CreateBluetooth();
    /** @brief 设置 → 设备 → 存储与 U 盘 (where coming back from Transfer lands) */
    static lv_obj_t* CreateStorage();
    /** @brief 创建设置→测试子页入口 */
    static lv_obj_t* CreateTest();
    /** @brief 切换语言后重建设置页文案与布局 */
    static void ReloadAfterLanguageChange();
};

/** @brief 进入待机时暂停设置页实时刷新（前光等） */
void SettingsScreen_OnEnterStandby();
/** @brief 退出待机后恢复设置页实时刷新（前光等） */
void SettingsScreen_OnResumeFromStandby();
/** @brief 在已打开的设置页里显示 网络 → 代理 */
void SettingsScreen_OpenProxy();
/** @brief Open 阅读与字库 (from the assistant page's 「书里提问另外设了」 row). */
void SettingsScreen_OpenReading();
