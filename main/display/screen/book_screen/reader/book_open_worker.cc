#include "reader_sync.h"
#include "book_screen/shelf/book_shelf_ops.h"

#include "assets/lang_reader.h"
#include "sd_paths.h"

#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/shelf/book_home.h"
#include "book_screen/reader/book_layout_worker.h"
#include "book_screen/settings/book_tap_zones.h"
#include "book_screen/book_text_util.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_reader_overlay.h"

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "book_screen/cover/book_cover_loader.h"
#include "book_screen/cover/book_cover_style.h"
#include "epd_content_policy.h"
#include "fontpack_lvgl.h"
#include "lv_adapter_display.h"
#include "reader/book_cover_sidecar.h"
#include "reader/reader.h"
#include "sd_access.h"
#include "screen_common.h"
#include "vk_key_handler.h"

namespace {
reader::RasterImage s_open_cover;
int s_last_open_pct = -1;
int64_t s_last_open_us = 0;

void DrawValetCoverPlaceholder(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const lv_coord_t cw = lv_area_get_width(&a);
    const lv_coord_t ch = lv_area_get_height(&a);
    if (cw < 20 || ch < 20) return;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_black();
    rect.bg_opa = LV_OPA_COVER;
    rect.border_width = 0;
    rect.radius = 0;

    // 粗书脊
    lv_area_t spine = {static_cast<lv_coord_t>(a.x1 + kCoverFrameBorder),
                       static_cast<lv_coord_t>(a.y1 + kCoverFrameBorder),
                       static_cast<lv_coord_t>(a.x1 + kCoverFrameBorder + 8),
                       static_cast<lv_coord_t>(a.y2 - kCoverFrameBorder)};
    lv_draw_rect(layer, &rect, &spine);

    // 三条标题横线（与首页 DrawBookCard 风格统一）
    const lv_coord_t line_x1 = a.x1 + 34;
    const lv_coord_t line_y0 = a.y1 + 48;
    for (int i = 0; i < 3; ++i) {
        const lv_coord_t lx2 = a.x2 - 24 - i * 18;
        if (lx2 > line_x1) {
            lv_area_t line_a = {line_x1, static_cast<lv_coord_t>(line_y0 + i * 24),
                                lx2, static_cast<lv_coord_t>(line_y0 + i * 24 + 2)};
            lv_draw_rect(layer, &rect, &line_a);
        }
    }
}
}  // namespace

struct OpenProgressMsg {
    uint32_t token = 0;
    int percent = 0;
};

struct OpenDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct OpenWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;  // 拷贝，避免 Create 重建书库时 vector 失效
};

// worker → Tick：勿 ScreenLvAsyncUrgent 抢锁（Tick 整屏 I1 时可占锁十余秒）
static std::atomic<OpenDoneMsg*> s_pending_open_done{nullptr};

static void AsyncOpenDone(void* user_data);

void BookDrainPendingOpenDone() {
    OpenDoneMsg* msg = s_pending_open_done.exchange(nullptr, std::memory_order_acq_rel);
    if (msg != nullptr) {
        if (Book_State().reader.standby_paused) {
            Book_State().reader.opening.store(false);
            s_pending_open_done.store(msg, std::memory_order_release);
            return;
        }
        AsyncOpenDone(msg);
    }
}

void ShowOpenProgress(int percent) {
    auto& st = Book_State();
    if (st.reader.content == nullptr) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    const bool need_build = st.reader.open_bar == nullptr || !lv_obj_is_valid(st.reader.open_bar) ||
                            st.reader.open_pct_lbl == nullptr || !lv_obj_is_valid(st.reader.open_pct_lbl);

    if (need_build) {
        s_last_open_pct = percent;
        s_last_open_us = esp_timer_get_time();
        st.reader.open_bar = nullptr;
        st.reader.open_pct_lbl = nullptr;
        lv_obj_clean(st.reader.content);
        s_open_cover.Reset();
        // 打开进度 UI 用书库边距；避免阅读页边距档位导致 Book_ContentWidth 溢出
        ApplyOverlayListContentPads(0);
        lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_NONE, 0);
        Book_DisableScroll(st.reader.content);

        const reader::BookInfo* book_ptr =
            (st.shelf.selected >= 0 && st.shelf.selected < static_cast<int>(st.shelf.books.size()))
                ? &st.shelf.books[static_cast<size_t>(st.shelf.selected)]
                : nullptr;

        lv_obj_t* panel = lv_obj_create(st.reader.content);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, Book_ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 20, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(panel);
        lv_obj_center(panel);

        // 1. 封面外框（优先旁路预览，无旁路则绘制极简书脊线框 + 格式角标）
        const lv_coord_t cover_w = UiSx(136);
        const lv_coord_t cover_h = UiSy(192);
        lv_obj_t* cover_frame = lv_obj_create(panel);
        lv_obj_remove_style_all(cover_frame);
        lv_obj_set_size(cover_frame, cover_w, cover_h);
        lv_obj_set_style_pad_all(cover_frame, kCoverFrameBorder, 0);
        lv_obj_set_style_bg_color(cover_frame, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cover_frame, LV_OPA_COVER, 0);
        StyleBookCoverFrame(cover_frame);
        lv_obj_clear_flag(cover_frame, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(cover_frame);

        bool has_cover = false;
        if (book_ptr != nullptr) {
            has_cover = BookCoverLoader_TryLoadBookSidecarPreview(
                            *book_ptr, CoverFrameInner(cover_w), CoverFrameInner(cover_h), s_open_cover) &&
                        !s_open_cover.empty();
        }
        if (has_cover) {
            s_open_cover.BindDsc();
            lv_obj_set_style_bg_color(cover_frame, lv_color_hex(0xF0F0F0), 0);
            lv_obj_t* img = lv_image_create(cover_frame);
            lv_image_set_src(img, &s_open_cover.dsc);
            EpdSetContentRole(img, EpdContentRole::Cover);
            lv_obj_center(img);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_event_cb(cover_frame, DrawValetCoverPlaceholder, LV_EVENT_DRAW_MAIN, nullptr);
            lv_obj_t* fmt_lbl = lv_label_create(cover_frame);
            lv_label_set_text(fmt_lbl, reader::FormatLabel(book_ptr ? book_ptr->format : st.reader.opening_format));
            lv_obj_set_style_text_font(fmt_lbl, Book_ItemFont(), 0);
            lv_obj_set_style_text_color(fmt_lbl, lv_color_black(), 0);
            lv_obj_align(fmt_lbl, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
            lv_obj_clear_flag(fmt_lbl, LV_OBJ_FLAG_CLICKABLE);
        }

        // 2. 书名（加粗，居中，最多两行）
        const lv_font_t* title_font = fontpack_lv_font_get(30, 4);
        if (title_font == nullptr) {
            title_font = Book_ListFont();
        }
        const char* book_title = (book_ptr && !book_ptr->title.empty()) ? book_ptr->title.c_str()
                                                                        : Lang::Strings::BOOK_OPENING;
        lv_obj_t* title = lv_label_create(panel);
        lv_label_set_text(title, book_title);
        lv_obj_set_width(title, Book_ContentWidth() - UiSx(32));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const lv_coord_t title_lh = (title_font && title_font->line_height > 0) ? title_font->line_height : 34;
        lv_obj_set_style_max_height(title, title_lh * 2 + 4, 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(title, title_font, 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        // 3. 进度区（直角细进度槽 + 状态文字）
        lv_obj_t* prog_col = lv_obj_create(panel);
        lv_obj_remove_style_all(prog_col);
        lv_obj_set_size(prog_col, Book_ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(prog_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(prog_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(prog_col, 10, 0);
        lv_obj_clear_flag(prog_col, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(prog_col);

        lv_coord_t bar_w = UiSx(220);
        if (bar_w > Book_ContentWidth() - 16) {
            bar_w = Book_ContentWidth() - 16;
        }
        lv_obj_t* bar = lv_bar_create(prog_col);
        lv_obj_set_size(bar, bar_w, UiSy(12));
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
        lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_bar = bar;

        lv_obj_t* pct = lv_label_create(prog_col);
        lv_obj_set_style_text_font(pct, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(pct, lv_color_black(), 0);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
        st.reader.open_pct_lbl = pct;
    }

    lv_bar_set_value(st.reader.open_bar, percent, LV_ANIM_OFF);
    char buf[48];
    if (percent > 0) {
        std::snprintf(buf, sizeof(buf), "正在打开 · %d%%", percent);
    } else {
        std::snprintf(buf, sizeof(buf), "正在打开…");
    }
    lv_label_set_text(st.reader.open_pct_lbl, buf);
}

static void AsyncOpenProgress(void* user_data) {
    auto* msg = static_cast<OpenProgressMsg*>(user_data);
    auto& st = Book_State();
    if (msg->token == st.reader.open_token.load() && st.reader.opening.load() && !st.reader.standby_paused &&
        st.reader.content != nullptr) {
        const int64_t now = esp_timer_get_time();
        const int diff = msg->percent - s_last_open_pct;
        if (msg->percent == 100 || s_last_open_pct < 0 ||
            (diff >= 12 && (now - s_last_open_us) >= 220000)) {
            s_last_open_pct = msg->percent;
            s_last_open_us = now;
            ShowOpenProgress(msg->percent);
        }
    }
    delete msg;
}

static void OnOpenProgress(int percent, void* user) {
    auto* msg = new OpenProgressMsg{};
    msg->token = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    msg->percent = percent;
    if (!ScreenLvAsync(AsyncOpenProgress, msg)) {
        delete msg;
    }
}

static void AsyncOpenDone(void* user_data) {
    auto* msg = static_cast<OpenDoneMsg*>(user_data);
    auto& st = Book_State();
    st.reader.opening.store(false);
    ESP_LOGI(TAG, "AsyncOpenDone begin ok=%d", msg->ok ? 1 : 0);

    if (msg->token != st.reader.open_token.load() || st.reader.deferred_cleanup || st.reader.content == nullptr) {
        FinishDeferredCleanup();
        delete msg;
        return;
    }

    if (!msg->ok || !st.reader.session || !st.reader.session->IsOpen()) {
        EndReadingTimeTracking();
        ReleaseBookFont();
        const auto error = st.reader.session ? st.reader.session->Error() : reader::BookReadError::kInvalidData;
        const char* message = Lang::Strings::BOOK_OPEN_FAILED_DATA;
        switch (error) {
            case reader::BookReadError::kFileIo: message = Lang::Strings::BOOK_OPEN_FAILED_IO; break;
            case reader::BookReadError::kNoMemory: message = Lang::Strings::BOOK_OPEN_FAILED_MEMORY; break;
            case reader::BookReadError::kCacheIo: message = Lang::Strings::BOOK_OPEN_FAILED_CACHE; break;
            case reader::BookReadError::kUnsupported: message = Lang::Strings::BOOK_OPEN_FAILED_UNSUPPORTED; break;
            case reader::BookReadError::kCancelled: message = Lang::Strings::BOOK_OPEN_CANCELLED; break;
            default: break;
        }
        Book_ShowMessage(st.reader.content, message);
        delete msg;
        return;
    }

    // 文档元数据回写书库，下次详情秒开可见 OPF 书名/作者
    if (st.shelf.selected >= 0 && st.shelf.selected < static_cast<int>(st.shelf.books.size()) &&
        st.shelf.books[static_cast<size_t>(st.shelf.selected)].path == st.reader.session->Info().path) {
        auto& bi = st.shelf.books[static_cast<size_t>(st.shelf.selected)];
        if (!st.reader.session->Title().empty()) {
            bi.title = st.reader.session->Title();
        }
        if (!st.reader.session->Author().empty()) {
            bi.author = st.reader.session->Author();
        }
    }

    const bool show_settings = st.reader.settings_resume_after_open;
    st.reader.settings_resume_after_open = false;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    RenderReaderPage();
    if (show_settings) ShowReadSettingsSheet(false);
    BeginReadingTimeTracking();
    valet::sync::PullOnOpen(st.reader.session->Info().path, st.reader.session->ReadingProgressX10());
    delete msg;
}

void OpenBookWorker(void* arg) {
    auto* ctx = static_cast<OpenWorkerArg*>(arg);
    auto& st = Book_State();
    bool ok = false;

    const uint32_t token_now = st.reader.open_token.load();
    ESP_LOGI(TAG, "book_open worker start token=%u fmt=%d path=%s",
             static_cast<unsigned>(ctx->token), static_cast<int>(ctx->info.format),
             ctx->info.path.c_str());

    if (st.reader.session == nullptr) {
        ESP_LOGE(TAG, "book_open skip: session null");
    } else if (ctx->token != token_now) {
        ESP_LOGW(TAG, "book_open skip: token stale %u!=%u", static_cast<unsigned>(ctx->token),
                 static_cast<unsigned>(token_now));
    } else {
        sd_access::Permit permit;
        st.reader.session->SetOpenProgress(OnOpenProgress,
                                           reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->token)));
        ok = st.reader.session->Open(ctx->info);
        st.reader.session->SetOpenProgress(nullptr, nullptr);
        // 打开路径统一补旁路封面，供书架/详情/目录取用（失败不阻断阅读）
        if (ok && ctx->token == st.reader.open_token.load() &&
            (ctx->info.format == reader::BookFormat::kEbook ||
             ctx->info.format == reader::BookFormat::kEpub)) {
            char sidecar[SD_FILE_PATH_CAPACITY];
            if (reader::BookCoverSidecarPath(ctx->info.path.c_str(), sidecar, sizeof(sidecar)) &&
                access(sidecar, R_OK) != 0) {
                reader::SaveBookCoverSidecar(ctx->info.path.c_str());
            }
        }
    }

    ESP_LOGI(TAG, "book_open worker done ok=%d", ok ? 1 : 0);

    auto* done = new OpenDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    // 无锁投递：Tick 持锁后先 BookDrainPendingOpenDone，再跑 timer_handler
    OpenDoneMsg* stale = s_pending_open_done.exchange(done, std::memory_order_acq_rel);
    if (stale != nullptr) {
        delete stale;
    }
    delete ctx;
    vTaskDeleteWithCaps(nullptr);
}

void StartOpenWorker(int book_index) {
    auto& st = Book_State();
    if (st.reader.standby_paused) return;
    if (st.reader.opening.load()) {
        const char* path = (book_index >= 0 && book_index < static_cast<int>(st.shelf.books.size()))
                               ? st.shelf.books[static_cast<size_t>(book_index)].path.c_str()
                               : "-";
        ESP_LOGW(TAG, "StartOpenWorker ignored: already opening token=%u req_idx=%d path=%s",
                 static_cast<unsigned>(st.reader.open_token.load()), book_index, path);
        return;
    }
    if (book_index < 0 || book_index >= static_cast<int>(st.shelf.books.size()) || !st.reader.session) {
        ESP_LOGE(TAG, "StartOpenWorker bad state idx=%d books=%u session=%d", book_index,
                 static_cast<unsigned>(st.shelf.books.size()), st.reader.session ? 1 : 0);
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED);
        return;
    }

    const uint32_t token = st.reader.open_token.fetch_add(1) + 1;
    st.reader.opening.store(true);
    st.reader.deferred_cleanup = false;
    st.reader.opening_format = st.shelf.books[static_cast<size_t>(book_index)].format;
    ShowOpenProgress(0);

    auto* ctx = new OpenWorkerArg{};
    ctx->token = token;
    ctx->info = st.shelf.books[static_cast<size_t>(book_index)];
    ESP_LOGI(TAG, "StartOpenWorker token=%u fmt=%d path=%s", static_cast<unsigned>(token),
             static_cast<int>(ctx->info.format), ctx->info.path.c_str());
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        OpenBookWorker, "book_open", kOpenWorkerStack, ctx, tskIDLE_PRIORITY + 2, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "book_open task failed: int free=%u spiram=%u stack=%u",
                 static_cast<unsigned>(free_int), static_cast<unsigned>(free_spiram),
                 static_cast<unsigned>(kOpenWorkerStack));
        delete ctx;
        st.reader.opening.store(false);
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_OPEN_FAILED_TASK);
    }
}

void StartReadAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = Book_State();
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    // 编排/打开 worker 仍握旧 session：勿 CreateReaderScreen 拆掉
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("start read");
        return;
    }
    st.shelf.selected = index;
    ESP_LOGI(TAG, "start read idx=%d path=%s", index,
             st.shelf.books[static_cast<size_t>(index)].path.c_str());
    CancelDetailCoverLoad();
    // 勿在此 Reset detail_cover：旧详情屏异步删除前仍引用封面缓冲
    ScreenLoadReplace(CreateReaderScreen(st.shelf.books[static_cast<size_t>(index)]));
}

void BackToLibraryAsync(void* /*user_data*/) {
    // 详情 vk_home / vk_prev → 书架或阅读首页。
    CancelDetailCoverLoad();
    // Create / CreateBookshelf 统一收尾正文 session / SD 字库（含 opening 中途离开的 deferred）。
    // detail_scr / cover 由详情屏 LV_EVENT_DELETE 清理，勿在此 Reset。
    if (Book_State().shelf.back_root == BookUiState::NavRoot::kShelf) {
        ScreenLoadReplace(CreateBookshelfScreen());
    } else {
        ScreenLoadReplace(BookScreen::Create());
    }
}

void BackToDetailAsync(void* /*user_data*/) {
    ReleaseReaderSessionForLeave();
    BackToLibraryAsync(nullptr);
}

void OpenDetailAsync(void* user_data) {
    CancelListCoverFill();
    CancelShelfDirectory();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = Book_State();
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        return;
    }
    st.shelf.selected = index;
    const char* screen = VkKey_ActiveScreen();
    if (screen != nullptr && std::strcmp(screen, kScreenBookshelf) == 0) {
        st.shelf.back_root = BookUiState::NavRoot::kShelf;
    } else {
        st.shelf.back_root = BookUiState::NavRoot::kHome;
    }
    StartReadAsync(user_data);
}

void OpenBookshelfAsync(void* /*user_data*/) {
    Book_State().shelf.list_page = 0;
    ScreenLoadReplace(CreateBookshelfScreen());
}

void BackToReadingHomeAsync(void* /*user_data*/) {
    ScreenLoadReplace(BookScreen::Create());
}

void OnContinueReadingClicked(lv_event_t* e) {
    auto& st = Book_State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("home continue CTA");
        return;
    }
    if (index < 0 || index >= static_cast<int>(st.shelf.books.size())) {
        ESP_LOGW(TAG, "home continue CTA bad index=%d books=%u", index,
                 static_cast<unsigned>(st.shelf.books.size()));
        return;
    }
    ESP_LOGI(TAG, "home continue CTA idx=%d path=%s", index,
             st.shelf.books[static_cast<size_t>(index)].path.c_str());
    st.shelf.back_root = BookUiState::NavRoot::kHome;
    st.shelf.selected = index;
    lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

void OnOpenShelfClicked(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (st.reader.opening.load()) {
        return;
    }
    lv_async_call(OpenBookshelfAsync, nullptr);
}

void OnDetailStartClicked(lv_event_t* /*e*/) {
    auto& st = Book_State();
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("detail continue CTA");
        return;
    }
    if (st.shelf.selected < 0 || st.shelf.selected >= static_cast<int>(st.shelf.books.size())) {
        ESP_LOGW(TAG, "detail continue CTA bad sel=%d books=%u", st.shelf.selected,
                 static_cast<unsigned>(st.shelf.books.size()));
        return;
    }
    ESP_LOGI(TAG, "detail continue CTA sel=%d path=%s", st.shelf.selected,
             st.shelf.books[static_cast<size_t>(st.shelf.selected)].path.c_str());
    lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(st.shelf.selected)));
}

void DispatchTapZoneAction(BookTapZoneAction action) {
    if (action == BookTapZoneAction::kPrev || action == BookTapZoneAction::kNext)
        ReaderAutoTurnManualInput();
    auto& st = Book_State();
    if (st.reader.opening.load() || !st.reader.session || !st.reader.session->IsOpen()) {
        return;
    }
    if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) {
        return;
    }
    switch (action) {
        case BookTapZoneAction::kPrev:
            QueueReaderPageTurn(-1);
            break;
        case BookTapZoneAction::kNext:
            QueueReaderPageTurn(1);
            break;
        case BookTapZoneAction::kMenu:
            ShowReadSettingsSheet();
            break;
        case BookTapZoneAction::kBack:
            // 与 vk_home 同路：勿在 CLICKED 栈里同步拆 session
            BookLvAsync(BackToDetailAsync);
            break;
        case BookTapZoneAction::kToc:
            OpenReadToc();
            break;
        case BookTapZoneAction::kBookmarks:
            OpenReadBookmarks();
            break;
        case BookTapZoneAction::kFontSettings:
            OpenReadFontSettings();
            break;
        case BookTapZoneAction::kSettings:
            OpenReadSettings();
            break;
        case BookTapZoneAction::kFullRefresh:
            if (auto* disp = LVAdapterDisplay::Instance()) {
                disp->RequestNextFullRefresh();
            }
            if (st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr)) {
                lv_obj_invalidate(st.reader.read_scr);
            } else if (st.reader.content != nullptr && lv_obj_is_valid(st.reader.content)) {
                lv_obj_invalidate(st.reader.content);
            }
            break;
        case BookTapZoneAction::kNone:
        default:
            break;
    }
}

