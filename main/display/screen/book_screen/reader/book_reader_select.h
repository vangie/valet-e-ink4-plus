#pragma once

// 正文长按选字（sim/ 新设计 → 正文 → 长按）：
//  - 长按一个字选中它（英文选中整个单词），手指不抬起接着滑，从这里往两边扩；
//  - 松手后两头各有一个把手（竖线加圆点），拖动改范围；
//  - 选中的字在正文位图里直接反显（黑底白字），旁边浮出卡片：上面是预设问题，点一下就问；
//    下面一排「查字典 / 整句 / 问 AI」。上面放得下就在选中的字上面，不然在下面。
//  - 点了问题：选中的字留着反显，在书页下方的悬窗里问（带上这一页和选中的字）；悬窗收起时去掉反显。
// 字的位置来自横排光栅（reader::GlyphBox）；竖排、插图页没有，长按不选字。只在 LVGL 线程调用。

#include <vector>

#include <lvgl.h>

#include "reader/reader.h"

/** @brief 正文画了新的一页：换上这一页的字位置，原来的选中作废（位图已经是新的）。page 为空 = 这一页不能选字 */
void ReaderSelectSetPage(std::vector<reader::GlyphBox> glyphs, const reader::Page* page);
/** @brief 去掉选中：恢复反显，收起把手和卡片 */
void ReaderSelectClear();
/** @brief 正在选字（有反显） */
bool ReaderSelectActive();
/** @brief 正文的点按先问这里：长按松手后跟着的那一下、有选中时点正文（收起选中）都吃掉，返回 true */
bool ReaderSelectConsumeClick();
/** @brief 正文触控区挂上长按、拖动 */
void ReaderSelectAttach(lv_obj_t* content);
void ReaderSelectDetach(lv_obj_t* content);
