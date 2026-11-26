// 书架 → 书库：浏览订阅的 OPDS 书库（目录一层层点进去）、跨书库搜书、下载到 books/ 后直接读。
// 网络和解析都在后台任务里，结果用 ScreenLvAsync 交回 UI；离开页面后旧结果按代次丢弃。
#include "book_screen/shelf/book_opds.h"

#include "assets/lang_config.h"
#include "book_screen/book_cloud_library.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "library_sources.h"
#include "opds_fetch.h"
#include "reader/book_library.h"
#include "reader/download_gate.h"
#include "reader/file_size.h"
#include "sd_paths.h"
#include "screen_common.h"
#include "screen_keyboard.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

namespace {
constexpr const char* TAG = "BookOpds";
constexpr uint32_t kFetchStack = 8 * 1024;
constexpr uint32_t kDownloadStack = 6 * 1024;   // 写 SD：内部 RAM 栈，与云书下载一致
constexpr lv_coord_t kRowH = UiSy(64);
constexpr lv_coord_t kHeadH = UiSy(40);
constexpr size_t kMaxFileTitleBytes = 120;

const char* Text(const char* zh, const char* en) { return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en; }
const lv_font_t* Strong() { const auto* f = fontpack_lv_font_get(30, 4); return f ? f : fontpack_lv_font_ui(); }
const lv_font_t* Small() { const auto* f = fontpack_lv_font_get(25, 2); return f ? f : fontpack_lv_font_ui(); }

// 一层目录（或一次搜索）的内容
struct Level {
    std::string url;      // 空 = 书库列表（订了多个时的第一层）或搜索结果
    std::string title;
    std::vector<valet::opds::Entry> entries;
    std::string next;     // 下一页
    int page = 0;
};

struct Ui {
    lv_obj_t* scr = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* footer = nullptr;
    lv_obj_t* editor = nullptr;
    lv_obj_t* edit_text = nullptr;
    lv_obj_t* detail = nullptr;
    lv_obj_t* action = nullptr;
    lv_obj_t* action_text = nullptr;
};

Ui s_ui;
std::vector<Level> s_stack;          // 浏览路径；最后一层在屏上
std::string s_query;                 // 最近一次搜索词（编辑框里的）
bool s_loading = false;
std::string s_error;
uint32_t s_gen = 0;                  // 每次换页面内容加一；后台结果代次不符就丢
int s_detail = -1;                   // 详情里的那本（当前层 entries 下标）
bool s_downloading = false;
int s_download_pct = 0;
DownloadGate s_gate;
// 每个书库的搜索模板和名字（拉过一次根目录就记下）
std::map<std::string, std::pair<std::string, std::string>> s_lib_meta;   // url → {search_template, title}

int RowsPerPage() {
    if (!s_ui.list) return 1;
    lv_obj_update_layout(s_ui.list);
    const lv_coord_t h = lv_obj_get_content_height(s_ui.list);
    return std::max(1, static_cast<int>(h / kRowH));
}
Level* Top() { return s_stack.empty() ? nullptr : &s_stack.back(); }
std::string LibrariesSignature(const std::vector<valet::LibrarySource>& libs) {
    std::string sig;
    for (const auto& lib : libs) sig += lib.url + "\n";
    return sig;
}
std::string s_signature;             // 订阅变了就从头开始

// ── 后台：拉 feed ─────────────────────────────────────────────────────────
enum class JobKind : uint8_t { kBrowse, kMore, kSearch };
struct SearchLib { std::string url, name, search_template; };
struct FetchJob {
    uint32_t gen = 0;
    JobKind kind = JobKind::kBrowse;
    std::string url;
    std::string query;
    std::vector<SearchLib> libs;
};
struct FetchDone {
    uint32_t gen = 0;
    JobKind kind = JobKind::kBrowse;
    bool ok = false;
    std::string error;
    std::string url;
    std::string feed_title;
    std::vector<valet::opds::Entry> entries;
    std::string next;
    std::vector<SearchLib> learned;   // 新知道的搜索模板
};

void ApplyFetch(void* p);

bool FetchParse(const std::string& url, valet::opds::Feed& feed, std::string& error) {
    auto got = valet::opds::FetchFeed(url);
    if (!got.ok) { error = got.error; return false; }
    if (!valet::opds::ParseFeed(got.body, url, feed)) {
        error = Text("这个地址返回的不是书库目录（OPDS）", "Not an OPDS catalog");
        return false;
    }
    if (got.truncated) feed.next.clear();   // 截断的一页没有可靠的「下一页」
    return true;
}

void FetchTask(void* arg) {
    auto* job = static_cast<FetchJob*>(arg);
    auto* done = new FetchDone{};
    done->gen = job->gen;
    done->kind = job->kind;
    done->url = job->url;
    if (job->kind == JobKind::kSearch) {
        int failed = 0;
        for (auto& lib : job->libs) {
            if (lib.search_template.empty()) {
                valet::opds::Feed root;
                std::string err;
                if (FetchParse(lib.url, root, err)) {
                    lib.search_template = root.search_template;
                    if (!lib.search_template.empty()) done->learned.push_back(lib);
                }
            }
            const std::string url = valet::opds::SearchUrl(lib.search_template, job->query);
            valet::opds::Feed feed;
            ESP_LOGD(TAG, "search tmpl=[%.80s] url=[%.80s]", lib.search_template.c_str(), url.c_str());
            if (url.empty() || !FetchParse(url, feed, done->error)) { ++failed; continue; }
            for (auto& entry : feed.entries) {
                if (entry.files.empty()) continue;   // 搜索结果里只要书
                entry.library = lib.name;
                done->entries.push_back(std::move(entry));
            }
        }
        done->ok = failed < static_cast<int>(job->libs.size()) || job->libs.empty();
        if (done->ok) done->error.clear();
        else if (done->error.empty()) done->error = Text("这个书库不支持搜索", "This library has no search");
    } else {
        valet::opds::Feed feed;
        done->ok = FetchParse(job->url, feed, done->error);
        if (done->ok) {
            done->feed_title = feed.title;
            done->entries = std::move(feed.entries);
            done->next = feed.next;
            if (!feed.search_template.empty()) done->learned.push_back({job->url, "", feed.search_template});
        }
    }
    ESP_LOGI(TAG, "fetch kind=%d ok=%d n=%u", static_cast<int>(done->kind), done->ok ? 1 : 0,
             static_cast<unsigned>(done->entries.size()));
    delete job;
    if (!ScreenLvAsync(ApplyFetch, done)) delete done;
    vTaskDelete(nullptr);
}

void Render();

void StartFetch(FetchJob* job) {
    job->gen = s_gen;
    s_loading = true;
    s_error.clear();
    if (xTaskCreatePinnedToCore(FetchTask, "opds_fetch", kFetchStack, job, tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        delete job;
        s_loading = false;
        s_error = Text("内存不够，稍后再试", "Out of memory, try again");
    }
    Render();
}

void FetchLevel(JobKind kind, const std::string& url) {
    auto* job = new FetchJob{};
    job->kind = kind;
    job->url = url;
    StartFetch(job);
}

void ApplyFetch(void* p) {
    auto* done = static_cast<FetchDone*>(p);
    for (const auto& lib : done->learned) {
        // kBrowse 时 lib.url = 访问的那一页 URL，不是订阅根 URL；
        // 把 search_template 同时存到所有订阅根 URL 里：
        // 只要 lib.url 是某个订阅的前缀，就更新那个订阅的缓存。
        auto& meta = s_lib_meta[lib.url];
        if (!lib.search_template.empty()) meta.first = lib.search_template;
        for (const auto& src : valet::LoadLibrarySources()) {
            if (lib.url == src.url || lib.url.find(src.url) == 0 || src.url.find(lib.url) == 0) {
                if (!lib.search_template.empty()) s_lib_meta[src.url].first = lib.search_template;
            }
        }
    }
    if (done->gen != s_gen || s_ui.scr == nullptr) { delete done; return; }
    s_loading = false;
    Level* top = Top();
    if (!done->ok) {
        s_error = done->error;
    } else if (top != nullptr) {
        if (done->kind == JobKind::kMore) {
            const int had = static_cast<int>(top->entries.size());
            for (auto& e : done->entries) top->entries.push_back(std::move(e));
            top->next = done->next;
            top->page = had / RowsPerPage();   // 停在新到的那一页
        } else {
            top->entries = std::move(done->entries);
            top->next = done->next;
            top->page = 0;
            if (top->title.empty()) top->title = done->feed_title;
        }
    }
    delete done;
    Render();
}
// ── 列表 ──────────────────────────────────────────────────────────────────
lv_obj_t* AddLabel(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

std::string FormatName(const valet::opds::Acquisition& file) {
    std::string ext = file.ext;
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

std::string EntryMeta(const valet::opds::Entry& entry, bool show_library) {
    std::string meta = entry.author;
    const auto* best = entry.Best();
    const auto* any = best ? best : (entry.files.empty() ? nullptr : &entry.files.front());
    if (any) {
        if (!meta.empty()) meta += " · ";
        meta += FormatName(*any);
        if (any->length) meta += " · " + reader::FormatCloudFileSize(any->length);
    }
    if (show_library && !entry.library.empty()) meta += " · " + entry.library;
    return meta;
}

// 文件名：书名去掉 FAT 不收的字符，截到 kMaxFileTitleBytes
std::string FileNameFor(const valet::opds::Entry& entry, const valet::opds::Acquisition& file) {
    std::string name;
    for (char c : entry.title) name.push_back(std::strchr("/\\:*?\"<>|", c) ? ' ' : c);
    name.resize(SdUtf8PrefixLength(name.c_str(), kMaxFileTitleBytes));
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) name = "book";
    return name + "." + file.ext;
}

reader::CloudBookTask TaskFor(const valet::opds::Entry& entry) {
    reader::CloudBookTask task;
    const auto* best = entry.Best();
    if (best == nullptr) return task;
    task.book_id = entry.id;
    task.title = entry.title;
    task.name = FileNameFor(entry, *best);
    task.format = best->ext == "txt" ? "TXT" : "EPUB";
    task.download_url = best->url;
    task.file_size = best->length;
    task.cover_image_url = entry.thumbnail.empty() ? entry.cover : entry.thumbnail;
    return task;
}

void OpenDetail(int index);

void OnRowClicked(lv_event_t* e) {
    if (s_loading) return;
    Level* top = Top();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (top == nullptr || index < 0 || index >= static_cast<int>(top->entries.size())) return;
    const auto& entry = top->entries[static_cast<size_t>(index)];
    if (!entry.files.empty()) { OpenDetail(index); return; }
    if (entry.nav.empty()) return;
    Level level;
    level.url = entry.nav;
    level.title = entry.title;
    s_stack.push_back(std::move(level));
    ++s_gen;
    FetchLevel(JobKind::kBrowse, s_stack.back().url);
}

void SetFooter(const char* text) {
    if (s_ui.footer && std::strcmp(lv_label_get_text(s_ui.footer), text) != 0) lv_label_set_text(s_ui.footer, text);
}

void Render() {
    if (s_ui.list == nullptr) return;
    Level* top = Top();
    if (s_ui.title) lv_label_set_text(s_ui.title, top && !top->title.empty() ? top->title.c_str() : Text("书库", "Libraries"));
    lv_obj_clean(s_ui.list);
    if (top == nullptr) return;
    const int rows = RowsPerPage();
    const int count = static_cast<int>(top->entries.size());
    if (count == 0 || (s_loading && top->entries.empty())) {
        const char* msg = s_loading ? Text("正在连书库…", "Connecting…")
                        : !s_error.empty() ? s_error.c_str()
                        : Text("这里没有书。换个词，或回上一层看看。", "Nothing here.");
        Book_ShowMessage(s_ui.list, msg, Book_ListFont());
        SetFooter(s_error.empty() ? "" : Text("按上一页键返回", "Press back"));
        return;
    }
    lv_obj_set_style_layout(s_ui.list, LV_LAYOUT_FLEX, 0);
    const int pages = (count + rows - 1) / rows;
    top->page = std::clamp(top->page, 0, pages - 1);
    const bool many = valet::LoadLibrarySources().size() > 1;
    // 这一层的宽度定死：书名很长（有的把整段宣传语写进了标题），靠固定宽 + 省略号收在一行
    const lv_coord_t row_w = Book_ContentWidth();
    const lv_coord_t tail_w = UiSx(56);
    const lv_coord_t text_w = row_w - tail_w - UiSx(8);
    const lv_coord_t title_h = Book_ItemFont()->line_height;
    const lv_coord_t meta_h = Small()->line_height;
    for (int i = top->page * rows; i < std::min(count, (top->page + 1) * rows); ++i) {
        const auto& entry = top->entries[static_cast<size_t>(i)];
        auto* row = lv_obj_create(s_ui.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, row_w, kRowH);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, UiSx(8), 0);
        Book_DisableScroll(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        auto* text = lv_obj_create(row);
        lv_obj_remove_style_all(text);
        lv_obj_set_size(text, text_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, 2, 0);
        Book_DisableScroll(text);
        lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
        auto* title = AddLabel(text, entry.title.c_str(), Book_ItemFont());
        lv_obj_set_size(title, text_w, title_h);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const bool book = !entry.files.empty();
        const std::string meta = book ? EntryMeta(entry, many) : std::string();
        if (!meta.empty()) {
            auto* sub = AddLabel(text, meta.c_str(), Small());
            lv_obj_set_size(sub, text_w, meta_h);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        }
        const char* tail = !book ? "\u203a" : TaskFor(entry).IsDownloaded() ? Text("在本机", "Here")
                         : entry.Best() == nullptr ? Text("读不了", "Unsupported") : "";
        if (tail[0]) {
            auto* hint = AddLabel(row, tail, book ? Small() : Book_ItemFont());
            lv_obj_set_size(hint, tail_w, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
            Book_DisableScroll(hint);
        }
    }
    char foot[48];
    const bool more = !top->next.empty();
    std::snprintf(foot, sizeof(foot), "%s%d/%d%s", s_loading ? "… " : "", top->page + 1, pages, more ? "+" : "");
    SetFooter(foot);
}
// ── 详情 + 下载 ───────────────────────────────────────────────────────────
void CloseDetail() {
    if (s_ui.detail) lv_obj_delete(s_ui.detail);   // DELETE 回调清指针
}

void OpenBook(const std::string& path, const std::string& title) {
    auto& st = Book_State();
    if (ReaderWorkersBusy()) { WarnReaderWorkersBusy("opds open"); return; }
    reader::BookInfo info;
    info.path = path;
    info.title = title.empty() ? reader::TitleFromPath(path.c_str()) : title;
    info.format = reader::DetectFormatByPath(path.c_str());
    st.shelf.books = {info};
    st.shelf.selected = 0;
    st.shelf.back_root = BookUiState::NavRoot::kShelf;
    lv_async_call(StartReadAsync, nullptr);
}

void UpdateAction() {
    if (!s_ui.action_text) return;
    Level* top = Top();
    if (!top || s_detail < 0 || s_detail >= static_cast<int>(top->entries.size())) return;
    const auto& entry = top->entries[static_cast<size_t>(s_detail)];
    const auto task = TaskFor(entry);
    char text[96];
    if (entry.Best() == nullptr) {
        std::snprintf(text, sizeof(text), "%s", Text("设备读不了这个格式", "Format not supported"));
    } else if (s_downloading) {
        std::snprintf(text, sizeof(text), Text("下载中 %d%%", "Downloading %d%%"), s_download_pct);
    } else if (task.IsDownloaded()) {
        std::snprintf(text, sizeof(text), "%s", Text("开始阅读", "Read"));
    } else if (entry.Best()->length) {
        std::snprintf(text, sizeof(text), "%s · %s", Text("下载到书架", "Download"),
                      reader::FormatCloudFileSize(entry.Best()->length).c_str());
    } else {
        std::snprintf(text, sizeof(text), "%s", Text("下载到书架", "Download"));
    }
    lv_label_set_text(s_ui.action_text, text);
}

int s_last_pct = -1;
struct DownloadJob { uint32_t gen; reader::CloudBookTask task; };
struct DownloadMsg { uint32_t gen; int pct; bool finished; bool ok; std::string error; std::string path; std::string title; };

void ApplyDownload(void* p) {
    auto* msg = static_cast<DownloadMsg*>(p);
    if (msg->finished) s_downloading = false;
    if (msg->gen == s_gen && s_ui.scr != nullptr) {
        if (!msg->finished) {
            s_download_pct = msg->pct;
        } else if (!msg->ok) {
            s_error = msg->error;
            if (s_ui.action_text) lv_label_set_text(s_ui.action_text, msg->error.c_str());
            delete msg;
            return;
        }
        UpdateAction();
        if (msg->finished && msg->ok) Render();
    }
    delete msg;
}

void DownloadProgress(int pct, void* user) {
    auto* job = static_cast<DownloadJob*>(user);
    if (pct == s_last_pct || (pct < 100 && s_last_pct >= 0 && pct - s_last_pct < 5)) return;
    s_last_pct = pct;
    auto* msg = new DownloadMsg{job->gen, pct, false, false, {}, {}, {}};
    if (!ScreenLvAsync(ApplyDownload, msg)) delete msg;
}

void DownloadTask(void* arg) {
    auto* job = static_cast<DownloadJob*>(arg);
    std::string err;
    const bool ok = reader::DownloadCloudBook(job->task, err, DownloadProgress, job, &s_gate);
    ESP_LOGI(TAG, "download ok=%d size=%llu err=[%s]", ok ? 1 : 0, static_cast<unsigned long long>(job->task.file_size), err.c_str());
    auto* msg = new DownloadMsg{job->gen, 100, true, ok, err, job->task.LocalPath(), job->task.title};
    delete job;
    if (!ScreenLvAsync(ApplyDownload, msg)) { delete msg; s_downloading = false; }
    vTaskDeleteWithCaps(nullptr);
}

void OnActionClicked(lv_event_t*) {
    Level* top = Top();
    if (!top || s_detail < 0 || s_detail >= static_cast<int>(top->entries.size()) || s_downloading) return;
    const auto& entry = top->entries[static_cast<size_t>(s_detail)];
    if (entry.Best() == nullptr) return;
    auto task = TaskFor(entry);
    if (task.LocalPath().empty()) return;
    if (task.IsDownloaded()) { OpenBook(task.LocalPath(), task.title); return; }
    auto* job = new DownloadJob{s_gen, std::move(task)};
    s_gate.Reset();
    s_downloading = true;
    s_download_pct = 0;
    s_last_pct = -1;
    if (xTaskCreatePinnedToCoreWithCaps(DownloadTask, "opds_dl", kDownloadStack, job, tskIDLE_PRIORITY + 2, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        delete job;
        s_downloading = false;
    }
    UpdateAction();
}

void OpenDetail(int index) {
    Level* top = Top();
    if (!top || s_ui.detail) return;
    s_detail = index;
    const auto& entry = top->entries[static_cast<size_t>(index)];
    auto* mask = ScreenPopupMask(s_ui.scr);
    s_ui.detail = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t*) { s_ui.detail = s_ui.action = s_ui.action_text = nullptr; s_detail = -1; },
                        LV_EVENT_DELETE, nullptr);
    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ScreenPopupTitleBar(sheet, Text("书的详情", "Book"), Strong(), Text("关闭", "Close"), Small(),
                        [](lv_event_t*) { lv_async_call([](void*) { CloseDetail(); }, nullptr); });
    auto* body = lv_obj_create(sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, 33, 0);
    lv_obj_set_style_pad_ver(body, 24, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    auto* title = AddLabel(body, entry.title.c_str(), Strong());
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    std::string meta = EntryMeta(entry, true);
    std::string formats;
    for (const auto& file : entry.files) formats += (formats.empty() ? "" : " / ") + FormatName(file);
    if (entry.files.size() > 1) meta += "\n" + std::string(Text("有 ", "Formats: ")) + formats;
    auto* sub = AddLabel(body, meta.c_str(), Small());
    lv_obj_set_width(sub, lv_pct(100));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
    if (!entry.summary.empty()) {
        auto* summary = AddLabel(body, entry.summary.c_str(), Small());
        lv_obj_set_width(summary, lv_pct(100));
        lv_obj_set_style_max_height(summary, UiSy(200), 0);
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_line_space(summary, 6, 0);
    }
    s_ui.action = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.action);
    lv_obj_set_size(s_ui.action, lv_pct(100), 84);
    lv_obj_set_style_bg_color(s_ui.action, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.action, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ui.action, 6, 0);
    lv_obj_add_flag(s_ui.action, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(s_ui.action);
    lv_obj_add_event_cb(s_ui.action, OnActionClicked, LV_EVENT_CLICKED, nullptr);
    s_ui.action_text = AddLabel(s_ui.action, "", Book_ItemFont());
    lv_obj_set_style_text_color(s_ui.action_text, lv_color_white(), 0);
    lv_obj_center(s_ui.action_text);
    UpdateAction();
}
// ── 搜索 ──────────────────────────────────────────────────────────────────
void CloseEditor() {
    if (s_ui.editor) lv_obj_delete(s_ui.editor);
}

void RunSearch() {
    if (s_query.empty()) return;
    auto* job = new FetchJob{};
    job->kind = JobKind::kSearch;
    job->query = s_query;
    for (const auto& lib : valet::LoadLibrarySources()) {
        const auto it = s_lib_meta.find(lib.url);
        job->libs.push_back({lib.url, lib.name, it == s_lib_meta.end() ? std::string() : it->second.first});
    }
    // 搜索结果是独立的一层，压在当前位置上面；返回键回到原来那层
    if (!s_stack.empty() && s_stack.back().url.empty() && s_stack.size() > 1) s_stack.pop_back();
    Level level;
    level.title = std::string(Text("搜「", "Search \"")) + s_query + Text("」", "\"");
    s_stack.push_back(std::move(level));
    ++s_gen;
    StartFetch(job);
}

void RefreshEditText() {
    if (!s_ui.edit_text) return;
    lv_label_set_text(s_ui.edit_text, s_query.empty() ? Text("书名、作者", "Title or author") : s_query.c_str());
}

void OpenEditor() {
    if (s_ui.editor || !s_ui.scr) return;
    auto* mask = ScreenPopupMask(s_ui.scr);
    s_ui.editor = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t*) { s_ui.editor = s_ui.edit_text = nullptr; }, LV_EVENT_DELETE, nullptr);
    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), UiSx(260) + 330);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ScreenPopupTitleBar(sheet, Text("搜书", "Search"), Strong(), Text("取消", "Cancel"), Small(),
                        [](lv_event_t*) { lv_async_call([](void*) { CloseEditor(); }, nullptr); });
    auto* panel = lv_obj_create(sheet);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_pad_hor(panel, 33, 0);
    lv_obj_set_style_pad_top(panel, 20, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    auto* hint = AddLabel(panel, Text("在订阅的书库里搜。书斋认拼音首字母（hlm = 红楼梦）。",
                                      "Searches every subscribed library."), Small());
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    auto* box = lv_obj_create(panel);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), 78);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_ui.edit_text = AddLabel(box, "", fontpack_lv_font_ui());
    lv_label_set_long_mode(s_ui.edit_text, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_ui.edit_text, lv_pct(100));
    lv_obj_align(s_ui.edit_text, LV_ALIGN_LEFT_MID, 0, 0);
    auto* keyboard = ScreenKeyboardCreate(panel, [](ScreenKeyboardEvent event, const char* text, void*) {
        switch (event) {
            case ScreenKeyboardEvent::kInsert:
                if (s_query.size() + std::strlen(text) < 80) s_query += text;
                break;
            case ScreenKeyboardEvent::kBackspace:
                while (!s_query.empty()) {
                    const unsigned char c = static_cast<unsigned char>(s_query.back());
                    s_query.pop_back();
                    if ((c & 0xC0) != 0x80) break;
                }
                break;
            case ScreenKeyboardEvent::kClear: s_query.clear(); break;
            case ScreenKeyboardEvent::kConfirm:
                if (!s_query.empty()) {
                    lv_async_call([](void*) { CloseEditor(); RunSearch(); }, nullptr);
                }
                return;
            case ScreenKeyboardEvent::kLayout: return;
        }
        RefreshEditText();
    });
    if (!keyboard) { CloseEditor(); return; }
    lv_obj_set_height(keyboard, 0);
    lv_obj_set_flex_grow(keyboard, 1);
    ScreenKeyboardReset(keyboard);
    RefreshEditText();
}

// ── 按键 ──────────────────────────────────────────────────────────────────
void GoBack() {
    if (s_ui.detail) { CloseDetail(); return; }
    if (s_ui.editor) { CloseEditor(); return; }
    Level* top = Top();
    if (top && top->page > 0) { --top->page; Render(); return; }
    if (s_stack.size() > 1) {
        s_stack.pop_back();
        ++s_gen;
        s_loading = false;
        s_error.clear();
        if (Top()->entries.empty() && !Top()->url.empty()) FetchLevel(JobKind::kBrowse, Top()->url);
        else Render();
        return;
    }
    s_gate.RequestStop();
    lv_async_call(OpenBookshelfAsync, nullptr);
}

void GoNext() {
    if (s_ui.detail || s_ui.editor || s_loading) return;
    Level* top = Top();
    if (!top) return;
    const int rows = RowsPerPage();
    const int pages = (static_cast<int>(top->entries.size()) + rows - 1) / rows;
    if (top->page + 1 < pages) { ++top->page; Render(); return; }
    if (!top->next.empty()) {
        FetchLevel(JobKind::kMore, top->next);
    }
}

bool OnKey(const char* key) {
    if (key == nullptr) return true;
    if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "vk_home") == 0) { lv_async_call([](void*) { GoBack(); }, nullptr); return true; }
    if (std::strcmp(key, "vk_next") == 0) { lv_async_call([](void*) { GoNext(); }, nullptr); return true; }
    return false;
}

void StartRoot() {
    const auto libs = valet::LoadLibrarySources();
    s_stack.clear();
    ++s_gen;
    s_error.clear();
    if (libs.empty()) {
        s_stack.push_back(Level{});
        s_error = Text("还没有订阅书库。到 设置 → 阅读与字库 → 书库订阅 里加。", "No library yet. Add one in Settings.");
        s_loading = false;
        Render();
        return;
    }
    if (libs.size() == 1) {
        Level level;
        level.url = libs.front().url;
        level.title = libs.front().name;
        s_stack.push_back(std::move(level));
        FetchLevel(JobKind::kBrowse, s_stack.back().url);
        return;
    }
    // 订了多个：第一层列书库，点进去是各自的目录
    Level level;
    level.title = Text("书库", "Libraries");
    for (const auto& lib : libs) {
        valet::opds::Entry entry;
        entry.title = lib.name;
        entry.nav = lib.url;
        level.entries.push_back(std::move(entry));
    }
    s_stack.push_back(std::move(level));
    s_loading = false;
    Render();
}
} // namespace

bool OpdsHasLibraries() { return !valet::LoadLibrarySources().empty(); }

void OnShelfLibraryClicked(lv_event_t*) {
    lv_async_call([](void*) { ScreenLoadReplace(CreateOpdsScreen()); }, nullptr);
}

lv_obj_t* CreateOpdsScreen() {
    ScreenSetIsHome(false);
    auto* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    s_ui = Ui{};
    s_ui.scr = scr;
    lv_obj_add_event_cb(scr, [](lv_event_t* e) {
        if (lv_event_get_target(e) != s_ui.scr) return;
        s_ui = Ui{};
        ++s_gen;
    }, LV_EVENT_DELETE, nullptr);
    EpdStatusBar status = ScreenCreateStatusBar(scr);
    if (status.status_label) lv_label_set_text(status.status_label, "");
    if (status.notification_label) lv_obj_add_flag(status.notification_label, LV_OBJ_FLAG_HIDDEN);

    auto* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height - kFooterH);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kListPad, 0);
    lv_obj_set_style_pad_row(body, UiSy(8), 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    Book_DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    auto* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), kHeadH);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Book_DisableScroll(head);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    s_ui.title = AddLabel(head, Text("书库", "Libraries"), Strong());
    lv_obj_set_flex_grow(s_ui.title, 1);
    lv_label_set_long_mode(s_ui.title, LV_LABEL_LONG_DOT);
    auto* search = lv_obj_create(head);
    lv_obj_remove_style_all(search);
    lv_obj_set_size(search, LV_SIZE_CONTENT, kHeadH);
    lv_obj_set_style_pad_hor(search, UiSx(10), 0);
    lv_obj_set_style_border_width(search, 2, 0);
    lv_obj_set_style_border_color(search, lv_color_black(), 0);
    lv_obj_set_style_radius(search, 4, 0);
    lv_obj_add_flag(search, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(search);
    lv_obj_add_event_cb(search, [](lv_event_t*) { if (!s_loading) OpenEditor(); }, LV_EVENT_CLICKED, nullptr);
    auto* search_text = AddLabel(search, Text("搜书", "Search"), Book_ItemFont());
    lv_obj_center(search_text);

    s_ui.list = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_width(s_ui.list, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_side(s_ui.list, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_ui.list, 2, 0);
    lv_obj_set_style_border_color(s_ui.list, lv_color_black(), 0);
    Book_DisableScroll(s_ui.list);
    lv_obj_clear_flag(s_ui.list, LV_OBJ_FLAG_CLICKABLE);

    s_ui.footer = lv_label_create(scr);
    lv_label_set_text(s_ui.footer, "");
    lv_obj_set_style_text_font(s_ui.footer, Book_ItemFont(), 0);
    lv_obj_align(s_ui.footer, LV_ALIGN_BOTTOM_MID, 0, -UiSy(8));

    VkKeyScreenDesc desc{};
    desc.factory = CreateOpdsScreen;
    desc.on_key = OnKey;
    VkKey_AttachScreen(scr, kScreenOpds, desc);

    const auto libs = valet::LoadLibrarySources();
    const std::string sig = LibrariesSignature(libs);
    if (sig != s_signature || s_stack.empty()) {
        s_signature = sig;
        StartRoot();
    } else {
        ++s_gen;
        s_loading = false;
        s_error.clear();
        if (Top()->entries.empty() && !Top()->url.empty()) FetchLevel(JobKind::kBrowse, Top()->url);
        else Render();
    }
    return scr;
}
