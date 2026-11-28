#include "book_screen/reader/book_page_work.h"
#include "assets/lang_reader.h"
#include "epd_content_policy.h"
#include "valet_reading_context.h"
#include "valet_book_tools.h"
#include "valet_protocol.h"
#include "chat_cloud.h"
#include "application.h"
#include "assistant_screen/assistant_screen.h"
#include "epd_refresh.h"
#pragma GCC optimize("O1")

#include "book_screen/reader/book_reader_body.h"
#include "book_screen/reader/book_auto_turn.h"
#include "book_screen/reader/book_reader_orient.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/shelf/book_detail.h"
#include "book_screen/settings/book_font_multi.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_geom.h"
#include "book_screen/reader/book_layout_debounce.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/reader/book_page_image.h"
#include "book_screen/reader/book_reader_prefs.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_toc_footer.h"
#include "book_screen/book_vk.h"
#include "book_screen/book_nav.h"
#include "book_screen/reader/book_reader_overlay.h"
#include "book_screen/reader/book_reader_select.h"

#include <lvgl.h>
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include <memory>
#include <cstring>
#include <utility>
#include <esp_log.h>

#include "assets/lang_config.h"
#include "book_screen/book_screen.h"
#include "display_orient.h"
#include "lv_adapter_display.h"
#include "reader/reader.h"
#include "reader/reader_page_render.h"
#include "reader/page_text_raster.h"
#include "reader/text_encoding.h"
#include "screen_common.h"
#include "vk_key_handler.h"

#include <esp_timer.h>

namespace {
bool s_reader_first_paint = false;

// 页上的文字行 → 助理用的行（段首 = 缩进、段前空或章名）
std::vector<valet::PageLine> ContextLines(const std::vector<reader::PageItem>& items) {
    std::vector<valet::PageLine> lines;
    for (const auto& item : items) {
        if (item.kind != reader::ContentKind::kText || item.text.empty()) continue;
        lines.push_back({item.text, item.para_indent || item.para_gap_before || item.chapter_title});
    }
    return lines;
}

// 悬窗上沿：在屏幕 1/4 高附近找正文位图里两行之间的空白，让悬窗不切过半行字。
// 位图是 L8（白 = 255）；找不到（整页是图、位图不在）就用 1/4 高。
// below：从选字进来时选中的字的下沿，悬窗开在它下面（只往下找空白），不盖住问的那几个字；
// 悬窗至少留 kMinPanelH 高，选中的字太靠下时就盖住一部分（悬窗里写着问的是哪几个字）。
constexpr lv_coord_t kMinPanelH = 520;
lv_coord_t ReaderPanelTop(lv_coord_t below = 0) {
    auto& st = Book_State();
    const bool keep_below = below > LV_VER_RES / 4 && below < LV_VER_RES - kMinPanelH;
    const lv_coord_t want = below > LV_VER_RES / 4 ? std::min<lv_coord_t>(below, LV_VER_RES - kMinPanelH) : LV_VER_RES / 4;
    auto* image = st.reader.text_raster_image;
    if (!image || !lv_obj_is_valid(image)) return want;
    auto* raster = static_cast<reader::RasterImage*>(lv_obj_get_user_data(image));
    if (!raster || raster->empty() || raster->dsc.header.cf != LV_COLOR_FORMAT_L8) return want;
    lv_area_t at;
    lv_obj_get_coords(image, &at);
    const int stride = raster->dsc.header.stride ? raster->dsc.header.stride : raster->width;
    const auto blank = [&](int row) {
        if (row < 0 || row >= raster->height) return false;
        const uint8_t* p = raster->pixels.data() + size_t(row) * stride;
        for (int x = 0; x < raster->width; ++x) if (p[x] < 0xF0) return false;
        return true;
    };
    constexpr int kGap = 6;   // 至少这么多行全白才算两行之间
    const int target = want - at.y1;
    for (int d = 0; d < LV_VER_RES / 8; ++d) {
        for (int row : {target - d, target + d}) {
            if (keep_below && row < target) continue;
            if (!blank(row)) continue;
            int start = row, end = row;
            while (blank(start - 1)) --start;
            while (blank(end + 1)) ++end;
            if (end - start + 1 < kGap) continue;
            return lv_coord_t(at.y1 + start + 1);   // 紧贴上一行下面：上面露出的都是整行
        }
    }
    return want;
}
// 悬窗上沿压着某一行时：往上找到这一行上面的空白，从那里盖白到上沿；没压着就返回上沿本身。
lv_coord_t ReaderPanelTrim(lv_coord_t top) {
    auto& st = Book_State();
    auto* image = st.reader.text_raster_image;
    if (!image || !lv_obj_is_valid(image)) return top;
    auto* raster = static_cast<reader::RasterImage*>(lv_obj_get_user_data(image));
    if (!raster || raster->empty() || raster->dsc.header.cf != LV_COLOR_FORMAT_L8) return top;
    lv_area_t at;
    lv_obj_get_coords(image, &at);
    const int stride = raster->dsc.header.stride ? raster->dsc.header.stride : raster->width;
    const auto blank = [&](int row) {
        if (row < 0 || row >= raster->height) return true;
        const uint8_t* p = raster->pixels.data() + size_t(row) * stride;
        for (int x = 0; x < raster->width; ++x) if (p[x] < 0xF0) return false;
        return true;
    };
    int row = top - 1 - at.y1;
    if (blank(row)) return top;
    while (row >= 0 && !blank(row)) --row;
    return lv_coord_t(at.y1 + row + 1);
}
}  // namespace

namespace {
// 书里提问和正在用的云服务不一样：先在主任务上换过去，换好了再开悬窗（问题也等那时再发）
struct PendingPanel { bool live = false; lv_coord_t below = 0; std::string question; };
PendingPanel* s_pending_panel = nullptr;
bool OpenPanelNow(bool live, lv_coord_t below, const std::string& question) {
    auto& st = Book_State();
    if (!st.reader.read_scr || !lv_obj_is_valid(st.reader.read_scr) || st.reader.read_scr != lv_screen_active()) return false;
    if (st.reader.read_chrome != BookUiState::ReadChrome::kReading) HideReadChrome();
    if (!AssistantScreen::OpenPanel(st.reader.read_scr, ReaderPanelTop(below), live)) return false;
    AssistantScreen::PanelTrimAbove(ReaderPanelTrim(AssistantScreen::PanelTop()));
    if (!question.empty()) Application::GetInstance().AskValetText(question);
    return true;
}
// 离开正文（不是去「全屏」接着这场对话）：云服务换回「谁来回答」选的那个
void RestoreSavedProvider(lv_event_t*) {
    if (AssistantScreen::PanelReturnPending()) return;
    if (chat_cloud::ActiveProvider() == chat_cloud::SavedProvider()) return;
    Application::GetInstance().Schedule([] { Application::GetInstance().UseProviderOnTask(chat_cloud::SavedProvider()); });
}
}  // namespace

bool OpenReaderAskPanel(bool live, lv_coord_t below, const std::string& question) {
    auto& st = Book_State();
    if (!st.reader.read_scr || !lv_obj_is_valid(st.reader.read_scr)) return false;
    if (AssistantScreen::PanelOpen()) {
        if (!question.empty()) Application::GetInstance().AskValetText(question);
        return true;
    }
    lv_obj_remove_event_cb(st.reader.read_scr, RestoreSavedProvider);
    lv_obj_add_event_cb(st.reader.read_scr, RestoreSavedProvider, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    const auto want = chat_cloud::BookProvider();
    if (want == chat_cloud::ActiveProvider()) return OpenPanelNow(live, below, question);
    if (s_pending_panel) return true;   // 已经在换了
    s_pending_panel = new PendingPanel{live, below, question};
    ESP_LOGI(TAG, "asking in a book: switch to provider %d first", static_cast<int>(want));
    const bool queued = Application::GetInstance().Schedule([want] {
        Application::GetInstance().UseProviderOnTask(want);
        ScreenLvAsync([](void*) {
            std::unique_ptr<PendingPanel> pending(s_pending_panel);
            s_pending_panel = nullptr;
            if (pending && !OpenPanelNow(pending->live, pending->below, pending->question)) ReaderSelectClear();
        });
    });
    if (!queued) { delete s_pending_panel; s_pending_panel = nullptr; return false; }
    return true;
}

namespace {
// 选中「这一页」时调用：读前后两页，把被翻页截断的首尾段落补全。阅读器忙、已关闭或读不到时返回空，用原来的本页文字。
std::pair<int, std::string> ExpandReadingText() {
    auto& st = Book_State();
    if (!st.reader.session || st.reader.opening.load() || ReaderPageWorkPending()) return {0, {}};
    const reader::Page* page = st.reader.session->CurrentPageData();
    if (!page) return {0, {}};
    const auto current = ContextLines(page->items);   // 先拷出来：读相邻页（TXT）会借用同一份页缓冲
    std::vector<reader::PageItem> items;
    const auto prev = st.reader.session->AdjacentPageItems(-1, items) ? ContextLines(items) : std::vector<valet::PageLine>{};
    const auto next = st.reader.session->AdjacentPageItems(+1, items) ? ContextLines(items) : std::vector<valet::PageLine>{};
    bool truncated = false;
    auto text = valet::ComposeReadingText(prev, current, next, 12000, truncated);
    ESP_LOGI("BookReader", "reading context: page %u B, before/after pages %u/%u lines, sent %u B%s",
             unsigned(valet::JoinPageLines(current).size()), unsigned(prev.size()), unsigned(next.size()),
             unsigned(text.size()), truncated ? " (page clipped)" : "");
    return {st.reader.session->BookCurrentPage() + 1, std::move(text)};
}

void FinishReaderFirstPaintGate() {
    if (!s_reader_first_paint) {
        return;
    }
    s_reader_first_paint = false;
    EpdRequestScreenChange(false);
    auto& st = Book_State();
    if (st.reader.read_scr != nullptr && lv_obj_is_valid(st.reader.read_scr)) {
        lv_obj_invalidate(st.reader.read_scr);
    }
}
}  // namespace

void OnReaderUnderlineDraw(lv_event_t* e) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (label == nullptr) {
        return;
    }
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(label)));
    if (mode != kBookReaderUnderlineSolid && mode != kBookReaderUnderlineDashed) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (layer == nullptr || font == nullptr || font->line_height <= 0) {
        return;
    }

    const char* txt = lv_label_get_text(label);
    if (!txt || !*txt) return;
    const auto* p = reinterpret_cast<const uint8_t*>(txt);
    size_t left = std::strlen(txt);
    const int letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int width = 0, ink_bottom = -1;
    // 正文和分行章名均为单行 label；同次字形查询同时取得宽度与最低墨迹边界。
    while (left > 0) {
        uint32_t cp = 0, next = 0;
        const size_t n = reader::Utf8Next(p, left, &cp);
        if (!n || cp == '\n') break;
        p += n;
        left -= n;
        reader::Utf8Next(p, left, &next);
        lv_font_glyph_dsc_t glyph{};
        lv_font_get_glyph_dsc(font, &glyph, cp, next);
        width += glyph.adv_w + letter_space;
        if (glyph.box_w > 0 && glyph.box_h > 0)
            ink_bottom = std::max(ink_bottom, reader::ReaderGlyphInkBottom(font, glyph));
    }
    width -= letter_space;
    if (width <= 0 || ink_bottom < 0) return;
    lv_area_t coords;
    lv_obj_get_content_coords(label, &coords);
    if (reader::IsJustifiedTextLabel(label) || reader::IsCompactTextLabel(label)) {
        width = reader::MakeTextLineSpacing(font, txt, lv_area_get_width(&coords),
            reader::IsJustifiedTextLabel(label), false, letter_space).width;
    }
    int x = coords.x1;
    const auto align = lv_obj_get_style_text_align(label, LV_PART_MAIN);
    if (align == LV_TEXT_ALIGN_CENTER) x += (lv_area_get_width(&coords) - width) / 2;
    else if (align == LV_TEXT_ALIGN_RIGHT) x += lv_area_get_width(&coords) - width;
    const int y = coords.y1 + reader::ReaderUnderlineTop(ink_bottom) + BookReaderPrefsTextValue(BookTextOption::UnderlineOffset);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_black();
    dsc.bg_opa = LV_OPA_COVER;
    const int step = mode == kBookReaderUnderlineDashed ? 10 : width;
    const int segment = mode == kBookReaderUnderlineDashed ? 6 : width;
    const int end_x = std::min<int>(x + width - 1, coords.x2);
    for (; x <= end_x; x += step) {
        lv_area_t area = {x, y, std::min(x + segment - 1, end_x),
                          y + reader::kReaderUnderlineThickness - 1};
        lv_draw_rect(layer, &dsc, &area);
    }
}

void AttachReaderUnderline(lv_obj_t* label, int mode) {
    if (label == nullptr || mode == kBookReaderUnderlineOff) {
        return;
    }
    lv_obj_set_user_data(label, reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
    lv_obj_add_event_cb(label, OnReaderUnderlineDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
}

void ShowReaderPageLoading() {
    auto& st = Book_State();
    if (!st.reader.content || st.reader.standby_paused) {
        return;
    }
    // 打开过渡卡片仍在屏幕上时保留之，避免闪一下单调的「加载中...」
    if (st.reader.open_bar != nullptr && lv_obj_is_valid(st.reader.open_bar)) {
        return;
    }
    CancelPageImageLoad();
    if (st.reader.background_layer && lv_obj_is_valid(st.reader.background_layer)) {
        lv_obj_delete(st.reader.background_layer);
    }
    st.reader.background_layer = nullptr;
    st.reader.background_image.reset();
    if (st.reader.footer_host) {
        lv_obj_set_style_bg_opa(st.reader.footer_host, LV_OPA_COVER, 0);
    }
    SyncReaderAaForImmersion(false);
    EpdSetContentRole(st.reader.content, EpdContentRole::Monochrome);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_LOADING);
}

void RenderReaderPage() {
    if (Book_State().reader.standby_paused) return;
    // Start a fresh interval after every render, including manual turns and layout changes.
    struct AutoTurnResetOnExit { ~AutoTurnResetOnExit() {
        if (ReaderAutoTurnEnabled()) ResetReaderAutoTurnDeadline();
    } } auto_turn_reset;
    if (ConsumeReaderAutoTurnRequest()) RequestReaderNavigation(reader::BookSession::NavigationKind::kDelta, 1);
    auto& st = Book_State();
    if (st.reader.content == nullptr || !st.reader.session) {
        s_reader_first_paint = false;
        return;
    }
    // Loading/errors/TOC stay binary; opt into text gray only when a page exists.
    EpdSetContentRole(st.reader.content,EpdContentRole::Monochrome);
    if (st.reader.read_chrome == BookUiState::ReadChrome::kToc) {
        st.reader.open_bar = nullptr;
        st.reader.open_pct_lbl = nullptr;
        s_reader_first_paint = false;
        SyncReaderAaForImmersion(false);
        RenderTocList();
        return;
    }
    // 设置卡片盖在正文上：重绘正文时保持卡片；其它情况回到沉浸
    const bool settings_overlay = (st.reader.read_chrome == BookUiState::ReadChrome::kSettings);
    SyncReaderAaForImmersion(true);
    if (!settings_overlay) {
        st.reader.read_chrome = BookUiState::ReadChrome::kReading;
        SetReadStatusVisible(false);
        SetReadSettingsSheetVisible(false);
    }
    const bool open_card_active = (st.reader.open_bar != nullptr && lv_obj_is_valid(st.reader.open_bar));
    if (!open_card_active) {
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        // 目录等会把 content 设成 flex；正文用绝对坐标居中，必须关掉 layout
        lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_NONE, 0);
    }

    const bool page_images = st.reader.session->IsPageImagesMode();
    const lv_coord_t vw = st.reader.session->ViewportWidth();
    const lv_coord_t vh = st.reader.session->ViewportHeight();
    const bool viewport_changed = vw != st.reader.viewport_w || vh != st.reader.viewport_h;
    // 分页视口与当前几何不一致：重排（宽或高任一过期都会让末行挤进底栏 / 行宽偏短）
    if (st.reader.viewport_w > 40 && st.reader.viewport_h > 40 && vw > 40 && vh > 40 &&
        viewport_changed && !st.reader.opening.load() &&
        !st.reader.layout_busy.load() && !st.reader.session->LiveRelayoutPending()) {
        ESP_LOGW(TAG, "viewport mismatch session=%dx%d ui=%dx%d → relayout",
                 static_cast<int>(vw), static_cast<int>(vh),
                 static_cast<int>(st.reader.viewport_w), static_cast<int>(st.reader.viewport_h));
        ApplyReaderLayoutLive(false);
        return;
    }

    // 视口尚未生效或章节仍在重排时，不能把旧页按新方向重新绘制。
    if (st.reader.layout_again || viewport_changed ||
        (st.reader.session->Info().format != reader::BookFormat::kTxt &&
         st.reader.session->LiveRelayoutPending() && !st.reader.session->NeedsCurrentPagePreview() &&
         !st.reader.session->HasCurrentPagePreview())) {
        if (!ReaderWorkersBusy() && st.reader.session->Error() != reader::BookReadError::kNone) {
            Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
        } else {
            ShowReaderPageLoading();
        }
        return;
    }

    auto prepared = TakeReaderPageResult();
    if (!prepared) {
        RequestReaderNavigation(reader::BookSession::NavigationKind::kRefresh);
        return;
    }
    st.reader.open_bar = nullptr;
    st.reader.open_pct_lbl = nullptr;
    if (open_card_active) {
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        lv_obj_set_style_layout(st.reader.content, LV_LAYOUT_NONE, 0);
    }
    struct RetiredFontRelease {
        ~RetiredFontRelease() { ReleaseRetiredBookFont(); }
    } release_font;
    CancelPageImageLoad();
    const reader::Page* page = st.reader.session->CurrentPageData();
    const bool reuse_raster = page && !page_images && BookReaderPrefsBackground() == 0 &&
        reader::PageIsTextOnly(*page) && st.reader.text_raster_image &&
        lv_obj_is_valid(st.reader.text_raster_image) && lv_obj_get_child_count(st.reader.content) == 1 &&
        lv_obj_get_child_count(lv_obj_get_parent(lv_obj_get_parent(st.reader.text_raster_image))) == 1;
    if (!reuse_raster) {
        lv_obj_clean(st.reader.content);
        Book_DisableScroll(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    if (page == nullptr) {
        ReaderSelectSetPage({}, nullptr);
        valet::PublishReadingPage({}, {}, {}, 0, {});
        FinishReaderFirstPaintGate();
        Book_ShowMessage(st.reader.content, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
        if (settings_overlay) {
            SetReadStatusVisible(true);
            SetReadSettingsSheetVisible(true);
        }
        return;
    }

    // 给助理的正文：页上的行拼回段落（不再一行一个换行）。前后被翻页截断的段落等真要提问时再补（见 ExpandReadingText）。
    valet::PublishReadingPage(st.reader.session->Info().path, st.reader.session->Title(),
        st.reader.session->CurrentTocTitle(), st.reader.session->BookCurrentPage() + 1,
        valet::JoinPageLines(ContextLines(page->items)));
    valet::SetReadingTextExpander(ExpandReadingText);
    {   // 给助理的书内搜索：哪本书、第几章、这一页第一行（只留在设备上）
        valet::BookSource source;
        source.path = st.reader.session->Info().path;
        source.book_id = valet::BookIdForPath(source.path);
        source.format = static_cast<int>(st.reader.session->Info().format);
        source.chapter = st.reader.session->SourceChapterIndex();
        for (const auto& item : page->items)
            if (item.kind == reader::ContentKind::kText && !item.text.empty()) { source.anchor = item.text; break; }
        valet::PublishBookSource(source);
    }
    // 问答悬窗开着：跟到这一页（画完再选，补全前后段落要读相邻页）；从「全屏」返回：悬窗接着开
    if (AssistantScreen::PanelOpen()) ScreenLvAsync([](void*) {
        AssistantScreen::PanelPageChanged();
        AssistantScreen::PanelTrimAbove(ReaderPanelTrim(AssistantScreen::PanelTop()));
    });
    else if (chat_cloud::IsCustom() && AssistantScreen::ConsumePanelReturn())
        ScreenLvAsync([](void*) { OpenReaderAskPanel(ValetProtocol::Live()); });

    if (prepared->direction != 0) {
        const int effect = BookReaderPrefsPageTurn();
        EpdSetReaderEffect(effect == kBookReaderPageTurnWipe ? EpdEffect::Wipe :
            effect == kBookReaderPageTurnRipple ? EpdEffect::Ripple : EpdEffect::None);
        EpdRequestPageTurn(prepared->direction);
    }
    EpdSetContentRole(st.reader.content,!page_images && (BookReaderPrefsAntialias() || BookReaderPrefsBackground() == 3)
        ? EpdContentRole::ReaderAntialias : EpdContentRole::Monochrome);
    const int64_t t0 = esp_timer_get_time();

    const lv_font_t* book_font = BookFont();
    const lv_coord_t max_w = st.reader.viewport_w;
    const auto content_area = ReadContentArea();
    const lv_coord_t content_h = st.reader.viewport_h;
    const int background = page_images ? 0 : BookReaderPrefsBackground();
    const bool background_changed = st.reader.background_image != prepared->background;
    if (background_changed && st.reader.background_layer && lv_obj_is_valid(st.reader.background_layer)) {
        lv_obj_delete(st.reader.background_layer);
        st.reader.background_layer = nullptr;
    }
    st.reader.background_image = prepared->background;
    if (background_changed && prepared->background) {
        auto* image = prepared->background.get();
        image->BindDsc();
        auto* slot = lv_obj_create(st.reader.read_scr);
        lv_obj_remove_style_all(slot);
        lv_obj_set_size(slot, image->width, image->height);
        lv_obj_set_pos(slot, 0, 0);
        st.reader.background_layer = slot;
        lv_obj_move_background(slot);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(slot);
        auto* picture = lv_image_create(slot);
        lv_image_set_src(picture, &image->dsc);
        EpdSetContentRole(picture, EpdContentRole::ReaderAntialias);
        lv_obj_clear_flag(picture, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(picture);
        auto* owner = new std::shared_ptr<reader::RasterImage>(prepared->background);
        lv_obj_add_event_cb(picture, [](lv_event_t* e) {
            delete static_cast<std::shared_ptr<reader::RasterImage>*>(lv_event_get_user_data(e));
        }, LV_EVENT_DELETE, owner);
    }
    st.reader.background_mode = background;
    const int spacing_percent = st.reader.session->LineSpacingPercent();
    const lv_coord_t para_gap = st.reader.session->ParaGap();

    const auto attach_image = [&](lv_obj_t* parent, const reader::PageItem& item, int col_w, int top) {
        const int img_h = item.image_h;
        lv_obj_t* slot = lv_obj_create(parent);
        lv_obj_remove_style_all(slot);
        lv_obj_set_width(slot, col_w);
        lv_obj_set_height(slot, page->items.size() == 1 ? content_h : img_h);
        if (top >= 0) lv_obj_set_pos(slot, 0, top);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(slot);

        reader::RasterImage cached;
        if (st.reader.session->TryCopyCachedPageImage(item.image_href, item.image_w, img_h, cached) && !cached.empty()) {
            auto* heap_img = new reader::RasterImage(std::move(cached));
            AttachPageImageToSlot(slot, heap_img, true);
        } else {
            lv_obj_t* label = lv_label_create(slot);
            lv_label_set_text(label, Lang::Strings::BOOK_LOADING);
            lv_obj_set_style_text_font(label, book_font, 0);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(label);
            BookUiState::PageImagePending pending;
            pending.href = item.image_href;
            pending.max_w = item.image_w;
            pending.max_h = img_h;
            pending.slot = slot;
            st.reader.page_image_pending.push_back(std::move(pending));
        }
    };

    // 正文统一预渲为 L8，插图按同一布局坐标异步填充。
    if (!page_images && book_font != nullptr && content_h > 40 &&
        max_w > 40 && !prepared->raster.empty()) {
        const lv_coord_t col_w = prepared->column_width;
        const lv_coord_t col_x = content_area.x1 + (max_w - col_w) / 2;
        const int ink_h = prepared->ink_height;

        auto* heap_img = reuse_raster
            ? static_cast<reader::RasterImage*>(lv_obj_get_user_data(st.reader.text_raster_image))
            : new reader::RasterImage();
        if (reuse_raster) {
            lv_image_cache_drop(&heap_img->dsc);
            lv_image_header_cache_drop(&heap_img->dsc);
        }
        *heap_img = std::move(prepared->raster);
        lv_obj_t* slot = reuse_raster ? lv_obj_get_parent(st.reader.text_raster_image) : nullptr;
        lv_obj_t* col = reuse_raster ? lv_obj_get_parent(slot) : lv_obj_create(st.reader.content);
        if (!reuse_raster) lv_obj_remove_style_all(col);
        lv_obj_set_size(col, col_w, ink_h);
        lv_obj_set_pos(col, col_x, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        Book_DisableScroll(col);

        if (!reuse_raster) {
            slot = lv_obj_create(col);
            lv_obj_remove_style_all(slot);
        }
        lv_obj_set_size(slot, col_w, ink_h);
        lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
        if (reuse_raster) {
            heap_img->BindDsc();
            lv_image_set_src(st.reader.text_raster_image, &heap_img->dsc);
            lv_obj_invalidate(st.reader.text_raster_image);
        } else {
            AttachPageImageToSlot(slot, heap_img, false);
            st.reader.text_raster_image = lv_obj_get_child(slot, 0);
            lv_obj_add_event_cb(st.reader.text_raster_image, [](lv_event_t* event) {
                auto& image = Book_State().reader.text_raster_image;
                if (image == lv_event_get_target(event)) image = nullptr;
            }, LV_EVENT_DELETE, nullptr);
        }
        EpdSetContentRole(st.reader.text_raster_image,BookReaderPrefsAntialias() || background == 3
            ? EpdContentRole::ReaderAntialias : EpdContentRole::Monochrome);
        ReaderSelectSetPage(std::move(prepared->glyphs), page);   // 长按选字：这一页每个字的位置

        size_t image_index = 0;
        for (const auto& item : page->items) {
            if (item.kind != reader::ContentKind::kText) {
                attach_image(col, item, col_w, prepared->image_tops[image_index++]);
            }
        }
        StartPageImageWorker();
        UpdateReadFooter();
        FinishReaderFirstPaintGate();
        ResetPointerLongPress();
        if (settings_overlay) {
            SetReadStatusVisible(true);
            SetReadSettingsSheetVisible(true);
        }
        ESP_LOGD(TAG, "RenderReaderPage ok %dx%d cost=%lldms", col_w, ink_h,
                 static_cast<long long>((esp_timer_get_time() - t0) / 1000));
        return;
    }

    // LVGL label/插图路径（不能选字）
    ReaderSelectSetPage({}, nullptr);
    if (reuse_raster && st.reader.text_raster_image) {
        lv_obj_clean(st.reader.content);
        AttachReadBodyInput(st.reader.content);
    }
    const lv_coord_t col_w = page_images ? max_w
        : reader::PageTextColumnWidth(*page, book_font, max_w, st.reader.session->Style());
    const lv_coord_t col_x = content_area.x1 + (max_w - col_w) / 2;

    lv_obj_t* col = lv_obj_create(st.reader.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, col_w, page_images ? content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_set_pos(col, col_x, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    Book_DisableScroll(col);

    const auto lines = reader::MakeTextLineLayout(book_font, content_h, spacing_percent, false, st.reader.session->Style().line_gap);
    lv_obj_set_style_text_letter_space(col, st.reader.session->Style().letter_gap, 0);
    int row_index = 0;
    bool prev_chapter_title = false;
    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            if (item.text.empty()) {
                continue;
            }
            const lv_coord_t gap =
                (item.para_gap_before && prev_chapter_title)
                    ? static_cast<lv_coord_t>(para_gap + reader::kReaderChapterTitleAfterGap)
                    : para_gap;
            reader::CreatePageTextLabel(col, item, col_w, book_font, lines, row_index, gap,
                [](lv_obj_t* label) { AttachReaderUnderline(label, BookReaderPrefsUnderlineMode()); }, st.reader.session->Style().indent_chars);
            prev_chapter_title = item.chapter_title;
        } else {
            prev_chapter_title = false;
            attach_image(col, item, col_w, -1);
        }
    }

    StartPageImageWorker();

    UpdateReadFooter();
    FinishReaderFirstPaintGate();
    // 重绘阻塞期间 tick 仍走：复位长按计时，避免恢复后一次判定就 LONG_PRESSED
    ResetPointerLongPress();
    if (settings_overlay) {
        SetReadStatusVisible(true);
        SetReadSettingsSheetVisible(true);
    }
}

lv_obj_t* CreateReaderScreen(const reader::BookInfo& info) {
    auto& st = Book_State();
    // 硬护栏：layout/open worker 仍用旧 session 时禁止 make_unique 拆掉（UAF）
    if (ReaderWorkersBusy()) {
        WarnReaderWorkersBusy("CreateReaderScreen");
        return BookScreen::Create();
    }
    if (st.reader.deferred_cleanup) {
        FinishDeferredCleanup();
        if (st.reader.deferred_cleanup) return BookScreen::Create();
    }
    s_reader_first_paint = true;
    StopReaderAutoOrient();
    DisplayUiSetOrient(ReaderDisplayOrient());
    // 仅打开正文时加载 SD 字体；界面控件用固件字
    EnsureBookFont();
    CancelReadFontScan();
    // 换新 session 前停旧 timer/墙钟，避免回调 UAF 或漏写本段秒数
    if (!RetireReaderSession()) {
        st.reader.deferred_cleanup = true;
        return BookScreen::Create();
    }
    st.reader.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.reader.session);
    st.reader.opening_format = info.format;

    st.reader.background_layer = nullptr;
    st.reader.background_image.reset();
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, Book_ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    Book_DisableScroll(scr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.reader.status_bar = status.bar;
    st.reader.status_overlay = status.overlay;
    st.reader.status_h = status.height;
    st.reader.status_label = status.status_label;
    if (status.status_label) {
        lv_label_set_text(status.status_label, "");
        lv_obj_add_flag(status.status_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (status.notification_label) {
        lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    // 沉浸阅读：默认藏顶栏，正文占满状态栏区域
    SetReadStatusVisible(false);

    st.reader.title_label = nullptr;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - ReaderFooterHeight());
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    Book_DisableScroll(body);
    AttachReadBodyInput(body);
    st.reader.content = body;
    st.reader.read_chrome = BookUiState::ReadChrome::kReading;
    st.reader.toc_list_page = 0;

    // 卡片下面的网点层：和其他弹出层一样，表示正文被盖住了；点它和点正文一样收起卡片。
    lv_obj_t* shade = ScreenPopupMask(scr);
    st.settings.settings_shade = shade;
    lv_obj_add_event_cb(shade, [](lv_event_t* ev) {
        lv_event_stop_bubbling(ev);
        BookLvAsync(AsyncHideReadChrome);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(shade, LV_OBJ_FLAG_HIDDEN);

    // 底部阅读浮层（默认隐藏；菜单打开后显示时长页）
    lv_obj_t* sheet = lv_obj_create(scr);
    st.settings.settings_sheet = sheet;
    lv_obj_remove_style_all(sheet);
    lv_obj_set_width(sheet, LV_HOR_RES - kSheetSidePad * 2);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, (st.reader.status_h > 0 ? st.reader.status_h : 40) + 10);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sheet, kSheetBorderW, 0);
    lv_obj_set_style_border_color(sheet, lv_color_black(), 0);
    lv_obj_set_style_radius(sheet, kSheetRadius, 0);
    lv_obj_set_style_pad_all(sheet, kSheetInnerPad, 0);
    lv_obj_set_style_pad_row(sheet, kSheetGap, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    Book_DisableScroll(sheet);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    // 点在卡片空白处不穿透到正文（避免误收）
    lv_obj_add_event_cb(
        sheet, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);

    // 阅读根容器内上下紧接，底栏固定高度，信息标签在槽内居中。
    lv_obj_t* foot_host = lv_obj_create(scr);
    lv_obj_remove_style_all(foot_host);
    lv_obj_set_size(foot_host, LV_HOR_RES, ReaderFooterHeight());
    lv_obj_align(foot_host, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(foot_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(foot_host, 0, 0);
    Book_DisableScroll(foot_host);
    lv_obj_clear_flag(foot_host, LV_OBJ_FLAG_CLICKABLE);
    st.reader.footer_host = foot_host;

    lv_obj_t* footer = lv_label_create(foot_host);
    lv_obj_set_width(footer, LV_HOR_RES - UiSx(16));
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(footer, "");
    lv_obj_center(footer);
    Book_DisableScroll(footer);
    st.reader.page_label = footer;
    auto make_side = [&](bool right) {
        lv_obj_t* side = lv_label_create(foot_host);
        lv_obj_set_style_text_font(side, Book_ItemFont(), 0);
        lv_obj_set_style_text_color(side, lv_color_black(), 0);
        lv_label_set_text(side, "");
        lv_obj_align(side, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, right ? -UiSx(14) : UiSx(14), 0);
        Book_DisableScroll(side);
        return side;
    };
    st.reader.footer_left = make_side(false);
    st.reader.footer_right = make_side(true);
    // 章节单独放在页眉，不跟页码挤在页脚。
    lv_obj_t* chapter = lv_label_create(scr);
    lv_obj_set_style_text_font(chapter, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(chapter, lv_color_black(), 0);
    lv_label_set_long_mode(chapter, LV_LABEL_LONG_DOT);
    lv_label_set_text(chapter, "");
    lv_obj_add_flag(chapter, LV_OBJ_FLAG_HIDDEN);
    Book_DisableScroll(chapter);
    st.reader.title_label = chapter;
    // 页眉第一行：章号。回目名太长，两行分开才看得清。
    lv_obj_t* chapter_num = lv_label_create(scr);
    lv_obj_set_style_text_font(chapter_num, Book_ItemFont(), 0);
    lv_obj_set_style_text_color(chapter_num, lv_color_black(), 0);
    lv_label_set_long_mode(chapter_num, LV_LABEL_LONG_DOT);
    lv_label_set_text(chapter_num, "");
    lv_obj_add_flag(chapter_num, LV_OBJ_FLAG_HIDDEN);
    Book_DisableScroll(chapter_num);
    st.reader.title_num = chapter_num;

    // 建屏时固定层叠顺序：正文、底栏、设置、状态栏；尺寸与显隐不再调整层级。
    lv_obj_move_foreground(sheet);
    lv_obj_move_foreground(status.bar);
    lv_obj_move_foreground(status.overlay);

    ShowOpenProgress(0);
    st.reader.read_scr = scr;
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_target(e) == Book_State().reader.read_scr) StopReaderAutoOrient();
    }, LV_EVENT_DELETE, nullptr);
    SyncReaderAutoOrient();
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    ApplyReaderChromeSize();
    SyncFooterClockTimer();
    st.reader.session->SetViewport(st.reader.viewport_w, st.reader.viewport_h);
    ScreenSetIsHome(false);
    // factory=ResumeReader：进百问压栈后返回依 .pos 续读；短按 HOME 回详情
    VkKey_AttachScreen(scr, kScreenRead, BookAiLongPressDesc(ResumeReaderScreen));
    StartOpenWorker(st.shelf.selected);
    return scr;
}

