#pragma once

#include <lvgl.h>
#include "reader/reader_types.h"

/** @brief 统一封面外框样式 */
void StyleBookCoverFrame(lv_obj_t* obj);
/** @brief 设置封面右上角 45° 斜角来源丝带标签（kLocal 为不显示） */
void SetBookCoverFrameSource(lv_obj_t* obj, reader::BookSource source);
