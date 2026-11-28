#pragma once

#include <lvgl.h>
#include "reader/reader_types.h"

/** @brief 统一封面外框样式 */
void StyleBookCoverFrame(lv_obj_t* obj);
/** @brief 设置封面右上角 45° 斜角来源丝带标签（kLocal 为不显示） */
void SetBookCoverFrameSource(lv_obj_t* obj, reader::BookSource source);
/** @brief 设置封面底边内嵌阅读进度条（pct_x10: 0..1000，<=0 不画） */
void SetBookCoverFrameProgress(lv_obj_t* obj, int pct_x10);
