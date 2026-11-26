#pragma once
// 书架上的「书库」：浏览订阅的 OPDS 书库（目录、搜索），把书下到 SD 卡 books/，下好就能读。
// 下好的书就是本机的书，书架上照常出现。

#include <lvgl.h>

constexpr const char* kScreenOpds = "book_opds"; // 在线书库

/** @brief 有没有订阅书库（书架标题行据此显示「书库」入口） */
bool OpdsHasLibraries();
/** @brief 创建书库页；返回时保留上次看到的位置 */
lv_obj_t* CreateOpdsScreen();
/** @brief 书架标题行「书库」按钮 */
void OnShelfLibraryClicked(lv_event_t* e);
