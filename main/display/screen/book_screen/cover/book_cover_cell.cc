#include "epd_content_policy.h"
#include "book_screen/cover/book_cover_cell.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/cover/book_cover_style.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_reader_prefs.h"

#include <lvgl.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "haptic_feedback.h"
#include "fontpack_lvgl.h"
#include "reader/reader.h"
#include "reader/list_thumbnail_cache.h"
#include "assets/lang_config.h"

namespace {
void OnCoverPlaceholderDraw(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    // 若封面图片已显示（第一个子控件 lv_image 非 hidden），则不画占位装饰
    if (lv_obj_get_child_count(obj) > 0) {
        lv_obj_t* first = lv_obj_get_child(obj, 0);
        if (first && lv_obj_check_type(first, &lv_image_class) && !lv_obj_has_flag(first, LV_OBJ_FLAG_HIDDEN)) {
            return;
        }
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const lv_coord_t cw = lv_area_get_width(&a);
    const lv_coord_t ch = lv_area_get_height(&a);
    if (cw < 32 || ch < 32) return;

    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;
    ink.border_width = 0;
    ink.radius = 0;

    // 左侧装订粗书脊（内缘与 DrawBookCoverBorder 的书脊压痕线融合）
    const lv_coord_t spine_w = 5;
    lv_area_t spine = {static_cast<lv_coord_t>(a.x1 + kCoverFrameBorder),
                       static_cast<lv_coord_t>(a.y1 + kCoverFrameBorder),
                       static_cast<lv_coord_t>(a.x1 + kCoverFrameBorder + spine_w - 1),
                       static_cast<lv_coord_t>(a.y2 - kCoverFrameBorder)};
    lv_draw_rect(layer, &ink, &spine);

    // 上部极简封面装饰横线（呼应首页阅读卡片风格）
    const lv_coord_t lx1 = a.x1 + kCoverFrameBorder + spine_w + 14;
    const lv_coord_t ly0 = a.y1 + ch / 5;
    for (int i = 0; i < 2; ++i) {
        const lv_coord_t lx2 = a.x2 - 28 - i * 18;
        if (lx2 > lx1 + 12) {
            lv_area_t la = {lx1, static_cast<lv_coord_t>(ly0 + i * 12),
                            lx2, static_cast<lv_coord_t>(ly0 + i * 12 + 1)};
            lv_draw_rect(layer, &ink, &la);
        }
    }
}
}  // namespace

lv_obj_t* CreateCoverSlot(lv_obj_t* parent, const reader::BookInfo& info, lv_coord_t w, lv_coord_t h,
                          reader::RasterImage* cover_out, bool* need_embed_fill, bool directory) {
    if (need_embed_fill != nullptr) {
        *need_embed_fill = false;
    }
    lv_obj_t* host = lv_obj_create(parent);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, w, h);
    lv_obj_set_style_pad_all(host, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(host, lv_color_hex(0xF8F8F8), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
    StyleBookCoverFrame(host);
    if (!directory && info.source != reader::BookSource::kLocal) {
        SetBookCoverFrameSource(host, info.source);
    }
    if (!directory) {
        lv_obj_add_event_cb(host, OnCoverPlaceholderDraw, LV_EVENT_DRAW_MAIN, nullptr);
    }
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(host);

    if (cover_out != nullptr) cover_out->Reset();
    lv_obj_t* img = lv_image_create(host);
    EpdSetContentRole(img,EpdContentRole::Cover);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* ph_lbl = lv_label_create(host);
    const lv_font_t* title_f = fontpack_lv_font_get(25, 2);
    if (!title_f) title_f = Book_ItemFont();
    const char* ph_text = directory
                              ? Lang::Strings::BOOK_FOLDER
                              : (!info.title.empty() ? info.title.c_str() : reader::FormatLabel(info.format));
    lv_label_set_text(ph_lbl, ph_text);
    lv_obj_set_style_text_font(ph_lbl, directory ? Book_ListFont() : title_f, 0);
    lv_obj_set_style_text_color(ph_lbl, lv_color_black(), 0);
    if (directory) {
        lv_obj_set_width(ph_lbl, std::max<int>(1, w - 2 * (kCoverSpineInset + kCoverFrameBorder + 8)));
        lv_label_set_long_mode(ph_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(ph_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(ph_lbl);
    } else {
        const lv_coord_t text_left = 18;
        const lv_coord_t text_w = std::max<int>(24, w - 2 * kCoverFrameBorder - text_left - 12);
        lv_obj_set_width(ph_lbl, text_w);
        const lv_coord_t lh = (title_f && title_f->line_height > 0) ? title_f->line_height : 28;
        lv_obj_set_style_max_height(ph_lbl, lh * 3 + 4, 0);
        lv_label_set_long_mode(ph_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(ph_lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(ph_lbl, LV_ALIGN_LEFT_MID, text_left, 6);

        // 右下角低调小号格式角标
        const lv_font_t* fmt_f = fontpack_lv_font_get(18, 2);
        if (!fmt_f) fmt_f = title_f;
        lv_obj_t* fmt_lbl = lv_label_create(host);
        lv_label_set_text(fmt_lbl, reader::FormatLabel(info.format));
        lv_obj_set_style_text_font(fmt_lbl, fmt_f, 0);
        lv_obj_set_style_text_color(fmt_lbl, lv_color_black(), 0);
        lv_obj_align(fmt_lbl, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
        lv_obj_clear_flag(fmt_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_clear_flag(ph_lbl, LV_OBJ_FLAG_CLICKABLE);
    bool cached = false;
    if (!directory && cover_out) {
        cached = reader::GetListThumbnail(info.path.c_str(), CoverFrameInner(w), CoverFrameInner(h),
            info.file_size, info.mtime, *cover_out);
        if (cached) {
            lv_image_set_src(img, &cover_out->dsc);
            lv_obj_center(img);
            lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
            for (uint32_t i = 1; i < lv_obj_get_child_count(host); ++i) {
                lv_obj_t* ch = lv_obj_get_child(host, i);
                if (lv_obj_check_type(ch, &lv_label_class)) {
                    lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
    if (need_embed_fill) *need_embed_fill=!directory && !cached && cover_out && !info.path.empty() &&
        !reader::ListThumbnailUnavailable(info.path.c_str(), info.file_size, info.mtime);
    return host;
}

// 书架 / 最近阅读：封面 + 可选书名
lv_obj_t* CreateBookCoverCell(lv_obj_t* parent, const reader::BookInfo& info, int index,
                              lv_coord_t cell_w, lv_coord_t cover_h,
                              std::vector<std::unique_ptr<reader::RasterImage>>& cover_store,
                              bool long_press_delete, lv_coord_t title_gap, bool show_title, bool directory) {
    auto& st = Book_State();
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, cell_w);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, show_title ? title_gap : 0, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(cell);
    lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(cell);
    lv_obj_add_event_cb(cell, directory ? OnShelfFolderClicked : OnBookRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    if (long_press_delete) {
        lv_obj_add_event_cb(cell, OnBookRowLongPressed, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    }

    auto cover = std::make_unique<reader::RasterImage>();
    bool need_embed_fill = false;
    lv_obj_t* host =
        CreateCoverSlot(cell, info, cell_w, cover_h, cover.get(), &need_embed_fill, directory);
    cover_store.push_back(std::move(cover));
    if (!directory && (need_embed_fill || st.shelf.back_root == BookUiState::NavRoot::kShelf)) {
        EnqueueListCoverFill(info, CoverFrameInner(cell_w), CoverFrameInner(cover_h), host,
                             cover_store.back().get());
    }

    const int pct_x10 = !directory && st.shelf.back_root != BookUiState::NavRoot::kShelf &&
                            BookReaderPrefsShowShelfProgress() != 0
                            ? Book_CachedProgressX10OrZero(info.path.c_str())
                            : -1;
    if (pct_x10 >= 0) {
        lv_obj_t* badge = Book_CreateProgressPctBadge(host, pct_x10);
        if (badge != nullptr) {
            // 相对书脊第二条竖线内侧再留空（外框 + 书脊 inset + 线宽 + 间隙）
            const lv_coord_t left =
                kCoverSpineInset + kCoverFrameBorder + 4;
            lv_obj_align(badge, LV_ALIGN_TOP_LEFT, left, 8);
        }
    }


    if (show_title) {
        const lv_font_t* item_font = Book_ItemFont();
        lv_obj_t* title = lv_label_create(cell);
        lv_obj_set_width(title, cell_w);
        lv_obj_set_style_text_font(title, item_font, 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const std::string clipped = TruncateTextToWidth(info.title, item_font, cell_w);
        lv_label_set_text(title, clipped.c_str());
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }
    if (long_press_delete && st.shelf.shelf_multi) {
        lv_obj_t* check = lv_obj_create(cell);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kShelfCoverCheckSize, kShelfCoverCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, 2, 0);
        lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(check,LV_OBJ_FLAG_FLOATING);
        lv_obj_add_flag(check,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(check,LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_pos(check,cell_w-kShelfCoverCheckSize/2,0);
        Book_DisableScroll(check);

        if (ShelfItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(mark, lv_color_black(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    return cell;
}

