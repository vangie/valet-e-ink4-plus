#include "assets/lang_reader.h"
#include "epd_content_policy.h"
#include "epd_refresh.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_opds.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/cover/book_cover_cell.h"
#include "book_screen/cover/book_cover_geometry.h"
#include "book_screen/cover/book_cover_workers.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/settings/book_ttf_panel.h"
#include "book_screen/book_vk.h"
#include "book_screen/book_nav.h"
#include "display_orient.h"
#include "book_screen/settings/book_settings_sheet.h"
#include "book_screen/settings/book_font_multi.h"

#include <lvgl.h>
#include <esp_timer.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "assets/lang_config.h"
#include "SdCardManager.hpp"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "screen_common.h"
#include "vk_key_handler.h"

namespace {
constexpr lv_coord_t kGalleryPageEdge = 4; // 右侧与下侧书页层叠边宽（2px 白 + 2px 黑）
static lv_coord_t ShelfSideInset() { return kShelfCoverCheckSize / 2; }
static lv_coord_t ShelfColumnGap() { return std::max<lv_coord_t>(kShelfColGap, kShelfCoverCheckSize / 2 + UiSx(8)); }
static lv_coord_t ShelfTitleBlockHeight() {
    return Book_ItemFont()->line_height + UiSy(4);
}

// 在 cell 上绘制封面右侧与下侧的「实体书页层叠边缘」（2px 纸白间隔 + 2px 墨线）
static void OnGalleryCellDraw(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t* cell = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!cell || !layer || lv_obj_get_child_count(cell) == 0) return;
    lv_obj_t* cover = lv_obj_get_child(cell, 0);
    if (!cover) return;
    lv_area_t ca;
    lv_obj_get_coords(cover, &ca);

    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;

    // 右侧书页厚度墨线（留 2px 纸白缝隙，再画 2px 墨线）
    lv_area_t right_edge = {static_cast<lv_coord_t>(ca.x2 + 3),
                            static_cast<lv_coord_t>(ca.y1 + 4),
                            static_cast<lv_coord_t>(ca.x2 + 4),
                            static_cast<lv_coord_t>(ca.y2 + 4)};
    lv_draw_rect(layer, &ink, &right_edge);

    // 底部书页厚度墨线
    lv_area_t bot_edge = {static_cast<lv_coord_t>(ca.x1 + 4),
                          static_cast<lv_coord_t>(ca.y2 + 3),
                          static_cast<lv_coord_t>(ca.x2 + 4),
                          static_cast<lv_coord_t>(ca.y2 + 4)};
    lv_draw_rect(layer, &ink, &bot_edge);
}
} // namespace

lv_coord_t ShelfBookWidth() {
    return (Book_ContentWidth()-2*ShelfSideInset()-ShelfColumnGap()*(kShelfCols-1))/kShelfCols;
}

void RenderBookshelfPage() {
    auto& st = Book_State();
    if (st.shelf.list_body == nullptr) {
        return;
    }
    CancelListCoverFill();

    if (!EnsureShelfDirectoryPage() || st.shelf.directory_failed) {
        if (st.shelf.directory_loading && !st.shelf.shelf_entries.empty() &&
            esp_timer_get_time() - st.shelf.directory_requested_us < 180000) return;
        lv_obj_clean(st.shelf.list_body);
        st.shelf.shelf_covers.clear();
        Book_ShowMessage(st.shelf.list_body, st.shelf.directory_failed ? Lang::Strings::DIRECTORY_READ_FAILED : Lang::Strings::BOOK_LOADING, Book_ListFont());
        return;
    }
    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        RenderBookshelfListPage();
        return;
    }

    lv_obj_set_style_layout(st.shelf.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(st.shelf.list_body, kShelfColGap, 0);

    Book_ClampListPage();
    lv_obj_clean(st.shelf.list_body);
    st.shelf.shelf_covers.clear();
    Book_DisableScroll(st.shelf.list_body);

    if (st.shelf.books.empty()) {
        lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
        char tip[160];
        std::snprintf(tip, sizeof(tip), Lang::Strings::BOOK_EMPTY_SHELF_FMT,
                      SdUserPath(reader::kDefaultBooksDir));
        Book_ShowMessage(st.shelf.list_body, tip, Book_ListFont());
        if (st.shelf.list_footer != nullptr) {
            lv_label_set_text(st.shelf.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t content_w = Book_ContentWidth();
    const lv_coord_t cell_w = ShelfBookWidth();
    const lv_coord_t cover_h =
        st.shelf.shelf_cover_h > 0 ? st.shelf.shelf_cover_h : (cell_w * 4 / 3);

    const int start = 0;
    const int end = std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.books.size()));
    const lv_coord_t title_block_h = ShelfTitleBlockHeight();
    for(int row_start=start;row_start<end;row_start+=kShelfCols) {
        auto* row=lv_obj_create(st.shelf.list_body);lv_obj_remove_style_all(row);
        lv_obj_set_size(row,content_w,cover_h+title_block_h+UiSy(6)+kShelfCoverCheckSize/2);
        Book_DisableScroll(row);lv_obj_clear_flag(row,LV_OBJ_FLAG_CLICKABLE);
        for(int col=0;col<kShelfCols && row_start+col<end;++col) {
            const int i=row_start+col;
            const bool is_dir = (st.shelf.shelf_entries[i].kind == reader::BookDirEntry::Kind::kFolder);
            auto* cell=CreateBookCoverCell(row,st.shelf.books[static_cast<size_t>(i)],i,cell_w,cover_h,
                                          st.shelf.shelf_covers,true,0,true, is_dir);
            lv_obj_set_style_layout(cell,LV_LAYOUT_NONE,0);
            lv_obj_add_event_cb(cell, OnGalleryCellDraw, LV_EVENT_DRAW_MAIN, nullptr);
            const lv_coord_t circle_inset=kShelfCoverCheckSize/2;
            lv_obj_set_size(cell,cell_w+circle_inset+kGalleryPageEdge,cover_h+title_block_h+UiSy(6)+circle_inset);
            lv_obj_set_pos(cell,ShelfSideInset()+col*(cell_w+ShelfColumnGap()),0);
            auto* cover=lv_obj_get_child(cell,0);lv_obj_set_pos(cover,0,circle_inset);
            auto* title=lv_obj_get_child(cell,1);
            const lv_coord_t lh = Book_ItemFont()->line_height;
            lv_obj_set_height(title,lh);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_set_pos(title,0,circle_inset+cover_h+UiSy(8));
        }
    }

    if (st.shelf.list_footer != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", st.shelf.list_page + 1, Book_ListPageCount());
        lv_label_set_text(st.shelf.list_footer, foot);
    }
    RefreshShelfFooterMode();
    EpdAfterNextSubmit(ScheduleListCoverFill);
}

lv_obj_t* CreateBookshelfScreen() {
    auto& st = Book_State();
    CancelShelfDirectory();
    ReleaseReaderSessionForLeave();
    st.shelf.shelf_multi = false;
    st.shelf.shelf_selected.clear();
    st.shelf.shelf_suppress_click_until_us = 0;
    st.shelf.shelf_suppress_click_idx = -1;
    st.shelf.multi_bar = nullptr;
    st.shelf.back_root = BookUiState::NavRoot::kShelf;
    CancelListCoverFill();
    st.shelf.recent_covers.clear();
    st.shelf.shelf_covers.clear();
    ScreenPaintCoalesceReset(&s_shelf_check_paint);

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    st.shelf.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = Book_State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.shelf.list_scr == tgt) {
                CancelShelfDirectory();
                CancelListCoverFill();
                s.shelf.list_scr = nullptr;
                s.shelf.list_body = nullptr;
                s.shelf.list_footer = nullptr;
                s.shelf.multi_bar = nullptr;
                s.shelf.shelf_title_lbl = nullptr;
                s.shelf.shelf_view_cover_btn = nullptr;
                s.shelf.shelf_view_list_btn = nullptr;
                s.shelf.shelf_multi = false;
                s.shelf.shelf_selected.clear();
                ScreenPaintCoalesceReset(&s_shelf_check_paint);
                s.shelf.recent_covers.clear();
                s.shelf.shelf_covers.clear();
                s.shelf.shelf_entries.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_label = status.status_label;
    if (status.status_label) {
        // 标题改在内容区「我的书架」行；状态栏留给多选计数
        lv_label_set_text(status.status_label, "");
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }

    BookReaderPrefsEnsureLoaded();
    st.shelf.shelf_view = BookReaderPrefsShelfView();
    if (st.shelf.shelf_dir.empty() || st.shelf.shelf_dir.rfind(reader::kDefaultBooksDir, 0) != 0) {
        st.shelf.shelf_dir = reader::kDefaultBooksDir;
    }
    st.shelf.shelf_entries.clear();
    st.shelf.shelf_title_lbl = nullptr;
    st.shelf.shelf_view_cover_btn = nullptr;
    st.shelf.shelf_view_list_btn = nullptr;

    const lv_coord_t body_h = LV_VER_RES - status.height - kFooterH;
    if (st.shelf.list_page < 0) {
        st.shelf.list_page = 0;
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kListPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kShelfHeaderGap, 0);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 标题行：左「我的书架」/当前文件夹，右封面|列表切换
    lv_obj_t* header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, Book_ContentWidth(), kShelfHeaderH);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(header);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title_lbl = lv_label_create(header);
    const lv_font_t* title_font = fontpack_lv_font_get(30, 4); // 加粗
    if (title_font == nullptr) {
        title_font = Book_ListFont();
    }
    lv_obj_set_style_text_font(title_lbl, title_font, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_set_flex_grow(title_lbl, 1);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
    st.shelf.shelf_title_lbl = title_lbl;

    // 书架一级入口：「搜书」直接跨书库搜书；「书库」用于配置与管理各书库源
    if (OpdsHasLibraries()) {
        auto make_head_btn = [&](const char* text, lv_event_cb_t cb) {
            lv_obj_t* btn = lv_obj_create(header);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, LV_SIZE_CONTENT, kShelfViewToggleH);
            lv_obj_set_style_pad_hor(btn, UiSx(10), 0);
            lv_obj_set_style_margin_right(btn, UiSx(8), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, lv_color_black(), 0);
            lv_obj_set_style_radius(btn, 4, 0);
            Book_DisableScroll(btn);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(btn);
            lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, text);
            lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            return btn;
        };
        const bool zh = std::strcmp(Lang::Current(), "zh-CN") == 0;
        make_head_btn(zh ? "搜书" : "Search", OnShelfSearchClicked);
        make_head_btn(zh ? "书库" : "Libraries", OnShelfLibraryClicked);
    }

    lv_obj_t* toggle = lv_obj_create(header);
    lv_obj_remove_style_all(toggle);
    lv_obj_set_height(toggle, kShelfViewToggleH);
    lv_obj_set_width(toggle, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(toggle, 2, 0);
    lv_obj_set_style_border_color(toggle, lv_color_black(), 0);
    lv_obj_set_style_radius(toggle, 4, 0);
    lv_obj_set_flex_flow(toggle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(toggle);
    lv_obj_clear_flag(toggle, LV_OBJ_FLAG_CLICKABLE);

    auto make_view_btn = [&](const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(toggle);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kShelfViewBtnW, kShelfViewBtnH);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(btn);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    st.shelf.shelf_view_cover_btn = make_view_btn(Lang::Strings::BOOK_VIEW_COVER, OnShelfViewCoverClicked);
    st.shelf.shelf_view_list_btn = make_view_btn(Lang::Strings::BOOK_VIEW_LIST, OnShelfViewListClicked);
    // 中间竖线
    lv_obj_t* sep = lv_obj_create(toggle);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, 1, 20);
    lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_to_index(sep, 1);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, Book_ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(list_body, kShelfColGap, 0);
    Book_DisableScroll(list_body);
    st.shelf.list_body = list_body;

    // 底栏槽：页码与多选共用，不改书籍区高度
    lv_obj_t* foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, kFooterH);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    Book_DisableScroll(foot);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* page_lbl = lv_label_create(foot);
    lv_obj_set_width(page_lbl, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page_lbl, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);
    lv_label_set_text(page_lbl, "1/1");
    lv_obj_align(page_lbl, LV_ALIGN_CENTER, 0, 0);
    Book_DisableScroll(page_lbl);
    st.shelf.list_footer = page_lbl;

    st.shelf.multi_bar = lv_obj_create(foot);
    lv_obj_remove_style_all(st.shelf.multi_bar);
    lv_obj_set_width(st.shelf.multi_bar, lv_pct(100));
    lv_obj_set_height(st.shelf.multi_bar, kShelfMultiBtnH);
    lv_obj_set_style_bg_opa(st.shelf.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.shelf.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.shelf.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.shelf.multi_bar, 10, 0);
    lv_obj_align(st.shelf.multi_bar, LV_ALIGN_CENTER, 0, 0);
    Book_DisableScroll(st.shelf.multi_bar);
    lv_obj_clear_flag(st.shelf.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.shelf.multi_bar, LV_OBJ_FLAG_HIDDEN);

    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kShelfMultiBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Book_DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
        lv_obj_set_style_pad_hor(text_wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        Book_DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kShelfMultiDotSize, kShelfMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        Book_DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.shelf.multi_bar, Lang::Strings::COMMON_CANCEL, OnShelfMultiCancel);
    make_multi_dot(st.shelf.multi_bar);
    make_multi_action(st.shelf.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnShelfMultiSelectAll);
    make_multi_dot(st.shelf.multi_bar);
    make_multi_action(st.shelf.multi_bar, Lang::Strings::COMMON_REMOVE, OnShelfMultiRemove);

    // 标题行以下可用高度：封面 3×3 / 列表行数共用
    st.shelf.shelf_content_h = body_h - kListPad * 2 - kShelfHeaderH - kShelfHeaderGap;
    if (st.shelf.shelf_content_h < 72) {
        st.shelf.shelf_content_h = 72;
    }

    if (st.shelf.shelf_view == kBookReaderShelfViewList) {
        ApplyShelfListPageSize();
        ReloadShelfDirListing();
    } else {
        st.shelf.list_page_size = kShelfCols * kShelfRows;
    }
    RefreshShelfViewToggleUi();
    UpdateShelfTitleLabel();

    const lv_coord_t cell_w = ShelfBookWidth();
    const lv_coord_t title_h = ShelfTitleBlockHeight() + UiSy(6) + kShelfCoverCheckSize / 2;
    const lv_coord_t usable = st.shelf.shelf_content_h;
    const lv_coord_t cell_budget =
        (usable - kShelfRowGap * (kShelfRows - 1)) / kShelfRows;
    lv_coord_t cover_h = cell_budget - title_h;
    const lv_coord_t cover_max = cell_w * 7 / 5;
    if (cover_h > cover_max) {
        cover_h = cover_max;
    }
    if (cover_h < 72) {
        cover_h = 72;
    }
    st.shelf.shelf_cover_h = cover_h;

    if (!SdCardManager::GetInstance().IsMounted()) {
        Book_ShowMessage(list_body, Lang::Strings::BOOK_NO_SD, Book_ListFont());
        VkKey_AttachScreen(scr, kScreenBookshelf, BookAiLongPressDesc(CreateBookshelfScreen));
        return scr;
    }

    // 当前目录按页加载，不依赖递归书库快照。
    ReloadShelfDirListing();
    RenderBookshelfPage();

    VkKey_AttachScreen(scr, kScreenBookshelf, BookAiLongPressDesc(CreateBookshelfScreen));
    return scr;
}

