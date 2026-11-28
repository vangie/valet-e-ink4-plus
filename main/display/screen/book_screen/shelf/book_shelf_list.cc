#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/shelf/book_shelf_ops.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/reader.h"
#include "reader/book_library.h"
#include "sd_directory_page.h"
#include "reader/list_thumbnail_cache.h"
#include "book_screen/cover/book_cover_style.h"
#include "sd_access.h"
#include "book_screen/cover/book_cover_workers.h"
#include "epd_refresh.h"
#include "weread_client.h"

bool ShelfDirIsRoot() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) {
        return true;
    }
    return st.shelf.shelf_dir == reader::kDefaultBooksDir;
}

std::string ShelfParentDir(const std::string& dir) {
    if (dir.empty() || dir == reader::kDefaultBooksDir) {
        return reader::kDefaultBooksDir;
    }
    std::string p = dir;
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        return reader::kDefaultBooksDir;
    }
    std::string parent = p.substr(0, slash);
    if (parent.empty() || parent.size() < std::strlen(reader::kDefaultBooksDir)) {
        return reader::kDefaultBooksDir;
    }
    // 不允许越出书库根
    if (parent.rfind(reader::kDefaultBooksDir, 0) != 0) {
        return reader::kDefaultBooksDir;
    }
    return parent;
}

const char* ShelfDirBasename(const std::string& dir) {
    if (dir.empty()) {
        return reader::kDefaultBooksDir;
    }
    const char* base = std::strrchr(dir.c_str(), '/');
    return (base != nullptr && base[1] != '\0') ? (base + 1) : dir.c_str();
}

int FindBookIndexByPath(const std::string& path) {
    auto& st = Book_State();
    for (size_t i = 0; i < st.shelf.books.size(); ++i) {
        if (st.shelf.books[i].path == path) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RefreshShelfViewToggleUi() {
    auto& st = Book_State();
    const bool cover = (st.shelf.shelf_view != kBookReaderShelfViewList);
    auto paint = [](lv_obj_t* btn, lv_obj_t* lbl, bool on) {
        if (btn == nullptr || !lv_obj_is_valid(btn)) {
            return;
        }
        lv_obj_set_style_bg_color(btn, on ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (lbl != nullptr && lv_obj_is_valid(lbl)) {
            lv_obj_set_style_text_color(lbl, on ? lv_color_white() : lv_color_black(), 0);
        }
    };
    lv_obj_t* cover_lbl =
        (st.shelf.shelf_view_cover_btn != nullptr) ? lv_obj_get_child(st.shelf.shelf_view_cover_btn, 0) : nullptr;
    lv_obj_t* list_lbl =
        (st.shelf.shelf_view_list_btn != nullptr) ? lv_obj_get_child(st.shelf.shelf_view_list_btn, 0) : nullptr;
    paint(st.shelf.shelf_view_cover_btn, cover_lbl, cover);
    paint(st.shelf.shelf_view_list_btn, list_lbl, !cover);
}

void UpdateShelfTitleLabel() {
    auto& st = Book_State();
    if (st.shelf.shelf_title_lbl == nullptr || !lv_obj_is_valid(st.shelf.shelf_title_lbl)) {
        return;
    }
    const std::string title = ShelfDirIsRoot() ? Lang::Strings::BOOK_SHELF_TITLE :
        "~" + st.shelf.shelf_dir.substr(std::strlen(reader::kDefaultBooksDir));
    const char* raw = title.c_str();
    // 留给右侧「搜书」「书库」与封面|列表切换
    const lv_coord_t toggle_w = kShelfViewBtnW * 2 + 1 + UiSx(188);
    const lv_coord_t max_w = Book_ContentWidth() - toggle_w;
    const lv_coord_t budget = max_w > 40 ? max_w : 40;
    lv_obj_set_width(st.shelf.shelf_title_lbl, budget);
    lv_label_set_long_mode(st.shelf.shelf_title_lbl, LV_LABEL_LONG_DOT);
    Book_SetFilenameMiddle(st.shelf.shelf_title_lbl, raw);
}

static SdDirectoryPage s_shelf_directory;
static lv_timer_t* s_shelf_directory_timer = nullptr;
static std::string s_requested_dir;
static int s_requested_page = -1;
static int s_requested_size = 0;
static uint32_t s_directory_revision = 0;
static bool ShelfDirectoryFilter(const char* name, bool directory) {
    return directory || reader::DetectFormatByPath(name) != reader::BookFormat::kUnknown;
}

void CancelShelfDirectory() {
    CancelShelfDelete();
    Book_State().shelf.screen_change_pending.store(false);
    s_shelf_directory.Cancel();
    reader::ClearListThumbnailCache();
    if (s_shelf_directory_timer) lv_timer_del(s_shelf_directory_timer);
    s_shelf_directory_timer = nullptr;
    s_requested_page = -1;
}

int ShelfDirectoryPages() { return s_shelf_directory.Pages(); }

static void ShelfDirectoryTick(lv_timer_t*) {
    auto& st = Book_State();
    if (!st.shelf.list_scr || !st.shelf.list_body ||
        st.shelf.back_root != BookUiState::NavRoot::kShelf || sd_access::IsQuiescing()) {
        CancelShelfDirectory();
        return;
    }
    SdDirectoryPage::Result result;
    const bool received = s_shelf_directory.Poll(result);
    if (!received && st.shelf.directory_loading && !st.shelf.directory_loading_shown &&
        esp_timer_get_time() - st.shelf.directory_requested_us >= 180000) {
        st.shelf.directory_loading_shown = true;
        RenderBookshelfPage();
    }
    if (received) {
        if (result.page != st.shelf.list_page) return;
        if (result.ok && result.entries.empty() && result.page > 0) {
            --st.shelf.list_page;
            RenderBookshelfPage();
            return;
        }
        CancelListCoverFill();
        lv_obj_clean(st.shelf.list_body);
        st.shelf.shelf_covers.clear();
        st.shelf.books.clear();
        st.shelf.shelf_entries.clear();
        for (auto& item : result.entries) {
            reader::BookDirEntry entry;
            entry.path = item.path;
            entry.name = item.path.substr(item.path.find_last_of('/') + 1);
            entry.kind = item.directory ? reader::BookDirEntry::Kind::kFolder : reader::BookDirEntry::Kind::kBook;
            entry.book.path = entry.path;
            entry.book.title = item.directory ? entry.name : reader::TitleFromPath(entry.name.c_str());
            entry.book.format = reader::DetectFormatByPath(entry.name.c_str());
            entry.book.file_size = item.bytes;
            entry.book.mtime = item.mtime;
            st.shelf.books.push_back(entry.book);
            st.shelf.shelf_entries.push_back(std::move(entry));
        }
        st.shelf.directory_loading = false;
        st.shelf.directory_failed = !result.ok;
        st.shelf.loaded_page = result.page;
        RenderBookshelfPage();
        if (!result.ok) { CancelShelfDirectory(); return; }
    }
    if (!st.shelf.directory_loading && st.shelf.list_footer) {
        if (s_shelf_directory.Counted()) {
            lv_label_set_text_fmt(st.shelf.list_footer, "%d/%d", st.shelf.list_page + 1, ShelfDirectoryPages());
            lv_timer_del(s_shelf_directory_timer);
            s_shelf_directory_timer = nullptr;
        } else {
            char text[32]; std::snprintf(text, sizeof(text), "%d/…", st.shelf.list_page + 1);
            if (std::strcmp(lv_label_get_text(st.shelf.list_footer), text)) lv_label_set_text(st.shelf.list_footer, text);
        }
    }
}

bool EnsureShelfDirectoryPage() {
    auto& st = Book_State();
    if (st.shelf.shelf_dir.empty()) st.shelf.shelf_dir = reader::kDefaultBooksDir;
    if (s_directory_revision != reader::BookLibraryRevision()) {
        s_directory_revision = reader::BookLibraryRevision();
        s_shelf_directory.Cancel();
        s_requested_page = -1;
        st.shelf.list_page = 0;
    }
    if (s_requested_dir != st.shelf.shelf_dir || s_requested_page != st.shelf.list_page ||
        s_requested_size != st.shelf.list_page_size) {
        if (s_requested_dir != st.shelf.shelf_dir) {
            if (!s_requested_dir.empty()) st.shelf.screen_change_pending.store(true);
            st.shelf.shelf_selected.clear();
            st.shelf.shelf_multi = false;
        }
        s_requested_dir = st.shelf.shelf_dir;
        s_requested_page = st.shelf.list_page;
        s_requested_size = st.shelf.list_page_size;
        st.shelf.directory_loading_shown = false;
        st.shelf.directory_requested_us = esp_timer_get_time();
        st.shelf.directory_loading = true;
        st.shelf.directory_failed = false;
        st.shelf.loaded_page = -1;
        CancelListCoverFill();
        s_shelf_directory.Request({s_requested_dir}, ShelfDirectoryFilter, s_requested_page, s_requested_size, false, nullptr, true);
        if (!s_shelf_directory_timer) s_shelf_directory_timer = lv_timer_create(ShelfDirectoryTick, 100, nullptr);
        if (!s_shelf_directory_timer) {
            st.shelf.directory_loading = false;
            st.shelf.directory_failed = true;
            ESP_LOGE(TAG, "directory page timer creation failed");
        }
    }
    const bool ready = !st.shelf.directory_loading && st.shelf.loaded_page == st.shelf.list_page;
    // 等待目录时仍保留旧控件，只有目标页就绪后才能消费翻页重刷请求。
    if (ready && !st.shelf.directory_failed && st.shelf.list_scr == lv_screen_active() &&
        st.shelf.screen_change_pending.exchange(false)) {
        EpdCancelPageTurn();
        EpdRequestScreenChange();
        lv_obj_invalidate(st.shelf.list_scr);
    }
    return ready;
}

void ReloadShelfDirListing() {
    s_requested_page = -1;
    s_shelf_directory.Cancel();
    EnsureShelfDirectoryPage();
    UpdateShelfTitleLabel();
}

// 扫盘 + 刷列表：须在 LVGL/异步任务跑，禁止 touch_feed 同步调用。
void ShelfReloadAndRenderAsync(void* /*user_data*/) {
    ESP_LOGD(TAG, "shelf directory page refresh");
    ReloadShelfDirListing();
    RenderBookshelfPage();
}

void ApplyShelfListPageSize() {
    auto& st = Book_State();
    int rows = 1;
    if (st.shelf.shelf_content_h > 0 && kShelfListRowH > 0) {
        rows = static_cast<int>(st.shelf.shelf_content_h / kShelfListRowH);
    }
    if (rows < 1) {
        rows = 1;
    }
    st.shelf.list_page_size = rows;
}

void OnShelfFolderClicked(lv_event_t* e) {
    if (Book_State().shelf.directory_loading) return;
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != reader::BookDirEntry::Kind::kFolder) {
        return;
    }
    st.shelf.shelf_dir = ent.path;
    st.shelf.list_page = 0;
    BookLvAsync(ShelfReloadAndRenderAsync);
}

void OnShelfListEntryLongPressed(lv_event_t* e) {
    if (Book_State().shelf.directory_loading) return;
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (!st.shelf.shelf_multi) {
        st.shelf.shelf_suppress_click_idx = idx;
        st.shelf.shelf_suppress_click_until_us = esp_timer_get_time() + kShelfSuppressRowClickUs;
        EnterShelfMultiModeSelect(idx);
        return;
    }
    ToggleShelfItemSelected(idx);
    RequestShelfCheckMarksPaint();
}

void OnShelfListBookClicked(lv_event_t* e) {
    if (Book_State().shelf.directory_loading) return;
    auto& st = Book_State();
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= static_cast<int>(st.shelf.shelf_entries.size())) {
        return;
    }
    if (idx == st.shelf.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf.shelf_multi) {
        ToggleShelfItemSelected(idx);
        RequestShelfCheckMarksPaint();
        return;
    }
    const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(idx)];
    if (ent.kind != reader::BookDirEntry::Kind::kBook) {
        return;
    }
    int book_index = FindBookIndexByPath(ent.path);
    if (book_index < 0) {
        st.shelf.books.push_back(ent.book);
        book_index = static_cast<int>(st.shelf.books.size()) - 1;
    }
    lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(book_index)));
}

void SetShelfViewMode(int view) {
    auto& st = Book_State();
    const int next = (view == kBookReaderShelfViewList) ? kBookReaderShelfViewList
                                                        : kBookReaderShelfViewCover;
    if (st.shelf.shelf_view == next) {
        RefreshShelfViewToggleUi();
        return;
    }
    if (st.shelf.shelf_multi) {
        ExitShelfMultiMode(false);
    }
    st.shelf.shelf_view = next;
    BookReaderPrefsSetShelfView(next);
    st.shelf.list_page = 0;
    CancelShelfDirectory();
    if (next == kBookReaderShelfViewList) {
        if (st.shelf.shelf_dir.empty()) {
            st.shelf.shelf_dir = reader::kDefaultBooksDir;
        }
        ApplyShelfListPageSize();
        ReloadShelfDirListing();
    } else {
        st.shelf.list_page_size = kShelfCols * kShelfRows;
        UpdateShelfTitleLabel();
    }
    RefreshShelfViewToggleUi();
    RequestRenderBookshelfPage(true);
}

void OnShelfViewCoverClicked(lv_event_t* /*e*/) {
    SetShelfViewMode(kBookReaderShelfViewCover);
}

void OnShelfViewListClicked(lv_event_t* /*e*/) {
    SetShelfViewMode(kBookReaderShelfViewList);
}

void RenderBookshelfListPage() {
    auto& st = Book_State();
    lv_obj_set_flex_flow(st.shelf.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.shelf.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.shelf.list_body, 0, 0);
    lv_obj_set_style_pad_column(st.shelf.list_body, 0, 0);

    Book_ClampListPage();
    lv_obj_clean(st.shelf.list_body);
    st.shelf.shelf_covers.clear();
    Book_DisableScroll(st.shelf.list_body);

    if (st.shelf.shelf_entries.empty()) {
        Book_ShowMessage(st.shelf.list_body, Lang::Strings::COMMON_EMPTY, Book_ListFont());
        if (st.shelf.list_footer != nullptr) {
            lv_label_set_text(st.shelf.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t row_w = Book_ContentWidth();
    const lv_font_t* item_font = Book_ItemFont();
    const lv_font_t* sub_font = fontpack_lv_font_get(25, 2);
    if (!sub_font) sub_font = item_font;
    const int start = 0;
    const int end =
        std::min(start + st.shelf.list_page_size, static_cast<int>(st.shelf.shelf_entries.size()));
    const lv_coord_t pct_col_w = Book_MeasureTextWidth(item_font, "100%");

    for (int i = start; i < end; ++i) {
        const auto& ent = st.shelf.shelf_entries[static_cast<size_t>(i)];
        const bool is_folder = (ent.kind == reader::BookDirEntry::Kind::kFolder);
        const bool selected = st.shelf.shelf_multi && ShelfItemSelected(i);

        lv_obj_t* row = lv_obj_create(st.shelf.list_body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, row_w, kShelfListRowH);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (selected) {
            lv_obj_set_style_bg_color(row, lv_color_hex(0xE8E8E8), 0);
        }
        if (i == start) {
            lv_obj_set_style_border_side(row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
        } else {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        }
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, kShelfListColGap, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(row);
        HapticAttachClick(row);
        lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_coord_t lead_used = 0;
        if (st.shelf.shelf_multi) {
            // 多选勾选框
            lv_obj_t* check = lv_obj_create(row);
            lv_obj_remove_style_all(check);
            lv_obj_set_size(check, kShelfCheckSize, kShelfCheckSize);
            lv_obj_set_style_bg_color(check, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(check, lv_color_black(), 0);
            lv_obj_set_style_border_width(check, kRowBorderW, 0);
            lv_obj_set_style_radius(check, 0, 0);
            Book_DisableScroll(check);
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            if (selected) {
                lv_obj_t* mark = lv_label_create(check);
                lv_label_set_text(mark, "√");
                lv_obj_set_style_text_font(mark, item_font, 0);
                lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                lv_obj_center(mark);
                lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
            }
            lead_used = kShelfCheckSize + kShelfListColGap;
        }

        // 单行紧凑排版（方案 3）：[书名 (去后缀)] [作者 (25px)] [来源小徽章] [百分比 / —]
        lv_obj_t* name = lv_label_create(row);
        lv_obj_set_style_text_font(name, item_font, 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* author_lbl = lv_label_create(row);
        lv_obj_set_style_text_font(author_lbl, sub_font, 0);
        lv_obj_set_style_text_color(author_lbl, lv_color_black(), 0);
        lv_label_set_long_mode(author_lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(author_lbl, "");
        lv_obj_add_flag(author_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(author_lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* pill = lv_obj_create(row);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, sub_font->line_height + 4);
        lv_obj_set_style_pad_hor(pill, 6, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_border_color(pill, lv_color_black(), 0);
        lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(pill);
        lv_obj_t* pill_lbl = lv_label_create(pill);
        lv_obj_set_style_text_font(pill_lbl, sub_font, 0);
        lv_label_set_text(pill_lbl, "");
        lv_obj_center(pill_lbl);
        lv_obj_clear_flag(pill_lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* tail = lv_label_create(row);
        lv_obj_set_width(tail, pct_col_w);
        lv_obj_set_style_text_align(tail, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(tail, item_font, 0);
        lv_obj_set_style_text_color(tail, lv_color_black(), 0);
        lv_obj_clear_flag(tail, LV_OBJ_FLAG_CLICKABLE);

        std::string title_src;
        if (is_folder) {
            title_src = ent.name + "/";
            lv_label_set_text(tail, ">");
        } else {
            title_src = !ent.book.title.empty() ? ent.book.title : reader::TitleFromPath(ent.name.c_str());
            lv_label_set_text(tail, "—");
            EnqueueListCoverFill(ent.book, 0, 0, row, nullptr);
        }
        const lv_coord_t name_w = std::max<lv_coord_t>(48, row_w - lead_used - pct_col_w - kShelfListColGap);
        lv_obj_set_width(name, name_w);
        lv_label_set_text(name, TruncateTextToWidth(title_src, item_font, name_w).c_str());

        lv_obj_add_event_cb(row, OnShelfListEntryLongPressed, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        if (is_folder) {
            lv_obj_add_event_cb(row, OnShelfFolderClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        } else {
            lv_obj_add_event_cb(row, OnShelfListBookClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));
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

void ApplyShelfEntryMetadata(const reader::BookInfo& info, lv_obj_t* host, bool progress_ready) {
    auto& st = Book_State();
    if (st.shelf.back_root != BookUiState::NavRoot::kShelf || st.shelf.directory_loading ||
        !st.shelf.list_scr || lv_obj_get_screen(host) != st.shelf.list_scr) {
        return;
    }
    const int index = FindBookIndexByPath(info.path);
    if (index < 0) {
        return;
    }
    const auto entry = std::find_if(st.shelf.shelf_entries.begin(), st.shelf.shelf_entries.end(),
        [&](const reader::BookDirEntry& item) { return item.kind == reader::BookDirEntry::Kind::kBook && item.path == info.path; });
    if (entry == st.shelf.shelf_entries.end()) return;
    st.shelf.books[index].file_size = info.file_size;
    st.shelf.books[index].mtime = info.mtime;
    st.shelf.books[index].source = info.source;
    if (!info.title.empty()) st.shelf.books[index].title = info.title;
    if (!info.author.empty()) st.shelf.books[index].author = info.author;
    entry->book.file_size = info.file_size;
    entry->book.mtime = info.mtime;
    entry->book.source = info.source;
    if (!info.title.empty()) entry->book.title = info.title;
    if (!info.author.empty()) entry->book.author = info.author;
    if (st.shelf.shelf_view == kBookReaderShelfViewCover) {
        const int pct_x10 = progress_ready
                                ? Book_CachedProgressX10OrZero(info.path.c_str())
                                : 0;
        SetBookCoverFrameProgress(host, pct_x10);
        SetBookCoverFrameSource(host, info.source);
        return;
    }

    // 列表视图（单行紧凑）：子控件顺序为 [check?], name, author_lbl, pill, tail
    const uint32_t base_idx = st.shelf.shelf_multi ? 1u : 0u;
    if (lv_obj_get_child_count(host) < base_idx + 4) return;
    auto* name = lv_obj_get_child(host, base_idx);
    auto* author_lbl = lv_obj_get_child(host, base_idx + 1);
    auto* pill = lv_obj_get_child(host, base_idx + 2);
    auto* tail = lv_obj_get_child(host, base_idx + 3);
    if (!name || !author_lbl || !pill || !tail) return;

    const lv_font_t* item_font = Book_ItemFont();
    const lv_font_t* sub_font = fontpack_lv_font_get(25, 2);
    if (!sub_font) sub_font = item_font;

    // 1. 右侧进度百分比（未读显示 —）
    char pct_txt[16] = "—";
    if (progress_ready) {
        const int pct_x10 = Book_CachedProgressX10OrZero(info.path.c_str());
        if (pct_x10 >= 1000) {
            std::snprintf(pct_txt, sizeof(pct_txt), "100%%");
        } else if (pct_x10 > 0) {
            std::snprintf(pct_txt, sizeof(pct_txt), "%d%%", std::max(1, (pct_x10 + 5) / 10));
        }
    }
    lv_label_set_text(tail, pct_txt);
    const lv_coord_t pct_col_w = lv_obj_get_width(tail);

    // 2. 来源小标签（微读：黑底白字；书库：白底黑框；本机：隐藏）
    const char* ribbon = valet::weread::BookSourceRibbonText(info.source);
    lv_coord_t pill_used = 0;
    if (ribbon && ribbon[0] != '\0') {
        const bool is_dark = (info.source == reader::BookSource::kWeRead);
        lv_obj_set_style_bg_color(pill, is_dark ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_t* pill_lbl = lv_obj_get_child(pill, 0);
        if (pill_lbl) {
            lv_label_set_text(pill_lbl, ribbon);
            lv_obj_set_style_text_color(pill_lbl, is_dark ? lv_color_white() : lv_color_black(), 0);
        }
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_HIDDEN);
        pill_used = Book_MeasureTextWidth(sub_font, ribbon) + 16 + kShelfListColGap;
    } else {
        lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);
    }

    // 3. 作者列（25px 字号，最多占行宽 28%）
    const lv_coord_t row_w = Book_ContentWidth();
    const lv_coord_t lead_used = st.shelf.shelf_multi ? (kShelfCheckSize + kShelfListColGap) : 0;
    lv_coord_t author_used = 0;
    if (!entry->book.author.empty()) {
        const lv_coord_t max_author_w = row_w * 28 / 100;
        const std::string clipped_author = TruncateTextToWidth(entry->book.author, sub_font, max_author_w);
        const lv_coord_t aw = std::min<lv_coord_t>(max_author_w, Book_MeasureTextWidth(sub_font, clipped_author.c_str()) + 4);
        lv_obj_set_width(author_lbl, aw);
        lv_label_set_text(author_lbl, clipped_author.c_str());
        lv_obj_clear_flag(author_lbl, LV_OBJ_FLAG_HIDDEN);
        author_used = aw + kShelfListColGap;
    } else {
        lv_obj_add_flag(author_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    // 4. 左侧书名（去后缀）
    const std::string clean_title = !entry->book.title.empty() ? entry->book.title : reader::TitleFromPath(entry->name.c_str());
    const lv_coord_t name_w = std::max<lv_coord_t>(
        48, row_w - lead_used - author_used - pill_used - pct_col_w - kShelfListColGap);
    lv_obj_set_width(name, name_w);
    lv_label_set_text(name, TruncateTextToWidth(clean_title, item_font, name_w).c_str());
}
