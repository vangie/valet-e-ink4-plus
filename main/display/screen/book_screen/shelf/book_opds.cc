// 书架 → 书库：浏览订阅的 OPDS 书库（目录一层层点进去）、跨书库搜书、下载到 books/ 后直接读。
// 网络和解析都在后台任务里，结果用 ScreenLvAsync 交回 UI；离开页面后旧结果按代次丢弃。
#include "book_screen/shelf/book_opds.h"

#include "application.h"
#include "assets/lang_config.h"
#include "book_screen/book_cloud_library.h"
#include "book_screen/book_font_runtime.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/book_text_util.h"
#include "book_screen/reader/book_open_worker.h"
#include "book_screen/shelf/book_bookshelf.h"
#include "epd_refresh.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "library_sources.h"
#include "opds_fetch.h"
#include "reader_sync.h"
#include "reader/book_library.h"
#include "reader/download_gate.h"
#include "reader/file_size.h"
#include "sd_paths.h"
#include "screen_common.h"
#include "screen_keyboard.h"
#include "ui_qrcode.h"
#include "valet_setup/server.h"
#include "vk_key_handler.h"
#include "weread_client.h"

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
    lv_obj_t* head_btn = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* footer = nullptr;
    lv_obj_t* editor = nullptr;
    lv_obj_t* edit_text = nullptr;
    lv_obj_t* detail = nullptr;
    lv_obj_t* action = nullptr;
    lv_obj_t* action_text = nullptr;
    lv_obj_t* qr_mask = nullptr;
    lv_obj_t* qr_code = nullptr;
    lv_obj_t* qr_status = nullptr;
    lv_obj_t* qr_sub = nullptr;
    lv_obj_t* setup_mask = nullptr;
    lv_obj_t* setup_qr = nullptr;
    lv_obj_t* setup_status = nullptr;
    lv_obj_t* setup_addr = nullptr;
    lv_timer_t* setup_timer = nullptr;
};

Ui s_ui;
bool s_open_search_immediately = false;
bool s_opened_from_search = false;
std::vector<Level> s_stack;          // 浏览路径；最后一层在屏上
std::string s_query;                 // 最近一次搜索词（编辑框里的）
bool s_loading = false;
std::string s_error;
uint32_t s_gen = 0;                  // 每次换页面内容加一；后台结果代次不符就丢
int s_detail = -1;                   // 详情里的那本（当前层 entries 下标）
bool s_downloading = false;
bool s_wish = false;              // 当前详情里那本是否「想读」
bool s_wish_pending = false;       // wish 请求进行中
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
    sig += valet::weread::IsLoggedIn() ? "wr:1:" + valet::weread::LoggedVid() : "wr:0";
    return sig;
}
std::string s_signature;             // 订阅变了就从头开始
uint32_t s_qr_gen = 0;               // 扫码登录轮询代次

// ── 后台：拉 feed ─────────────────────────────────────────────────────────
enum class JobKind : uint8_t { kBrowse, kMore, kSearch, kWeReadShelf };
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
    if (job->kind == JobKind::kWeReadShelf) {
        std::vector<valet::opds::Entry> shelf_books;
        done->ok = valet::weread::FetchShelf(shelf_books, done->error);
        if (done->ok) {
            done->feed_title = Text("微信读书 · 书架", "WeRead Shelf");
            for (auto& b : shelf_books) done->entries.push_back(std::move(b));
            valet::opds::Entry logout_entry;
            logout_entry.title = Text("退出微信读书登录", "Sign out of WeRead");
            logout_entry.author = Text("退出后仍可搜索全网并阅读免费试读章节", "Search & trial chapters remain available");
            logout_entry.nav = "weread://logout";
            done->entries.push_back(std::move(logout_entry));
        }
    } else if (job->kind == JobKind::kSearch) {
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
        // 同时搜索微信读书全网书库（支持免登录与已登录）
        std::vector<valet::opds::Entry> wr_entries;
        std::string wr_err;
        const bool wr_ok = valet::weread::SearchBooks(job->query, wr_entries, wr_err);
        if (wr_ok) {
            for (auto& e : wr_entries) done->entries.push_back(std::move(e));
        }
        done->ok = wr_ok || (failed < static_cast<int>(job->libs.size()) && !job->libs.empty());
        if (done->ok) done->error.clear();
        else if (!wr_err.empty()) done->error = wr_err;
        else if (done->error.empty()) done->error = Text("搜索失败，请检查网络连接", "Search failed");
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
void OpenWeReadQr();
void CloseWeReadQr();
void OpenSetupQr();
void CloseSetupQr();
void StartRoot();

void OnRowClicked(lv_event_t* e) {
    if (s_loading) return;
    Level* top = Top();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (top == nullptr || index < 0 || index >= static_cast<int>(top->entries.size())) return;
    const auto& entry = top->entries[static_cast<size_t>(index)];
    if (!entry.files.empty()) { OpenDetail(index); return; }
    if (entry.nav.empty()) return;
    if (entry.nav == "setup://library") {
        OpenSetupQr();
        return;
    }
    if (entry.nav == "weread://login") {
        OpenWeReadQr();
        return;
    }
    if (entry.nav == "weread://logout") {
        valet::weread::Logout();
        s_signature.clear();
        StartRoot();
        ScreenShowToast(Text("已退出微信读书", "Signed out of WeRead"));
        return;
    }
    if (entry.nav == "weread://shelf") {
        Level level;
        level.url = entry.nav;
        level.title = Text("微信读书 · 书架", "WeRead Shelf");
        s_stack.push_back(std::move(level));
        ++s_gen;
        FetchLevel(JobKind::kWeReadShelf, "weread://shelf");
        return;
    }
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
    const bool at_root = (s_stack.size() <= 1 && (!top || top->url.empty()) && (!top || top->title == Text("书库", "Libraries")));
    if (s_ui.title) lv_label_set_text(s_ui.title, top && !top->title.empty() ? top->title.c_str() : Text("书库", "Libraries"));
    if (s_ui.head_btn) {
        if (at_root) lv_obj_add_flag(s_ui.head_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_ui.head_btn, LV_OBJ_FLAG_HIDDEN);
    }
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
        const bool show_lib = many || !entry.library.empty();
        const std::string meta = book ? EntryMeta(entry, show_lib) : entry.author;
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
struct DownloadJob {
    uint32_t gen;
    reader::CloudBookTask task;
    std::string author;
};
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
    bool ok = false;
    if (job->task.download_url.rfind("weread://", 0) == 0) {
        const std::string wr_book_id = job->task.download_url.substr(9);
        ok = valet::weread::DownloadBookAsEpub(wr_book_id, job->task.title, job->author,
                                               job->task.cover_image_url, job->task.LocalPath(),
                                               err, DownloadProgress, job, &s_gate);
    } else {
        ok = reader::DownloadCloudBook(job->task, err, DownloadProgress, job, &s_gate);
        if (ok) {
            std::string sync_url = valet::sync::ProgressUrlFromDownload(job->task.download_url);
            if (sync_url.empty()) sync_url = "opds://";
            valet::sync::Remember(job->task.LocalPath(), sync_url);
        }
    }
    ESP_LOGI(TAG, "download ok=%d size=%llu err=[%s]", ok ? 1 : 0, static_cast<unsigned long long>(job->task.file_size), err.c_str());
    auto* msg = new DownloadMsg{job->gen, 100, true, ok, err, job->task.LocalPath(), job->task.title};
    delete job;
    if (!ScreenLvAsync(ApplyDownload, msg)) { delete msg; s_downloading = false; }
    vTaskDeleteWithCaps(nullptr);
}

void StartEntryDownload(const valet::opds::Entry& entry, bool force_overwrite) {
    auto task = TaskFor(entry);
    if (task.LocalPath().empty()) return;
    if (!force_overwrite && task.IsDownloaded()) {
        if (task.download_url.rfind("weread://", 0) == 0) {
            valet::sync::HintBeforeOpen(task.LocalPath(), task.download_url);
        } else {
            valet::sync::HintBeforeOpen(task.LocalPath(), valet::sync::ProgressUrlFromDownload(task.download_url));
        }
        OpenBook(task.LocalPath(), task.title);
        return;
    }
    auto* job = new DownloadJob{s_gen, std::move(task), entry.author};
    s_gate.Reset();
    s_downloading = true;
    s_download_pct = 0;
    s_last_pct = -1;
    const uint32_t stack = job->task.download_url.rfind("weread://", 0) == 0 ? (12 * 1024) : kDownloadStack;
    if (xTaskCreatePinnedToCoreWithCaps(DownloadTask, "opds_dl", stack, job, tskIDLE_PRIORITY + 2, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        delete job;
        s_downloading = false;
    }
    UpdateAction();
}

void OnActionClicked(lv_event_t*) {
    Level* top = Top();
    if (!top || s_detail < 0 || s_detail >= static_cast<int>(top->entries.size()) || s_downloading) return;
    const auto& entry = top->entries[static_cast<size_t>(s_detail)];
    if (entry.Best() == nullptr) return;
    StartEntryDownload(entry, false);
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

    // 微信读书已在本机时，提供「更新全本章节」按钮（方便免登录试读后扫码登录同步已购全本）
    const std::string dl_url = entry.files.empty() ? "" : entry.files.front().url;
    if (dl_url.rfind("weread://", 0) == 0 && TaskFor(entry).IsDownloaded()) {
        auto* upd_btn = lv_obj_create(body);
        lv_obj_remove_style_all(upd_btn);
        lv_obj_set_size(upd_btn, lv_pct(100), 72);
        lv_obj_set_style_border_width(upd_btn, 2, 0);
        lv_obj_set_style_border_color(upd_btn, lv_color_black(), 0);
        lv_obj_set_style_radius(upd_btn, 6, 0);
        lv_obj_add_flag(upd_btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(upd_btn);
        lv_obj_add_event_cb(upd_btn, [](lv_event_t*) {
            Level* top = Top();
            if (!top || s_detail < 0 || s_detail >= static_cast<int>(top->entries.size()) || s_downloading) return;
            StartEntryDownload(top->entries[static_cast<size_t>(s_detail)], true);
        }, LV_EVENT_CLICKED, nullptr);
        const int ch_n = valet::weread::DownloadedChapterCount(TaskFor(entry).LocalPath());
        char upd_txt[96];
        if (ch_n > 0) {
            std::snprintf(upd_txt, sizeof(upd_txt), Text("重新下载 / 更新章节（当前 %d 章）", "Redownload (%d chs)"), ch_n);
        } else {
            std::snprintf(upd_txt, sizeof(upd_txt), "%s", Text("重新下载 / 更新全本章节", "Redownload chapters"));
        }
        auto* upd_lbl = AddLabel(upd_btn, upd_txt, Small());
        lv_obj_center(upd_lbl);
    }

    // 「想读」按钮：只在有进度地址（带短码的书库）时显示
    const std::string progress_url = valet::sync::ProgressUrlFromDownload(dl_url);
    if (!progress_url.empty()) {
        s_wish = false;
        s_wish_pending = false;
        auto* wish_btn = lv_obj_create(body);
        lv_obj_remove_style_all(wish_btn);
        lv_obj_set_size(wish_btn, lv_pct(100), 72);
        lv_obj_set_style_border_width(wish_btn, 2, 0);
        lv_obj_set_style_border_color(wish_btn, lv_color_black(), 0);
        lv_obj_set_style_radius(wish_btn, 6, 0);
        lv_obj_add_flag(wish_btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(wish_btn);
        // 把进度地址复制到 user_data；detail 关闭时 object 被删就跟着释放
        auto* url_copy = new std::string(progress_url);
        lv_obj_set_user_data(wish_btn, url_copy);
        lv_obj_add_event_cb(wish_btn, [](lv_event_t* e) {
            delete static_cast<std::string*>(lv_obj_get_user_data(lv_event_get_target_obj(e)));
        }, LV_EVENT_DELETE, nullptr);
        // 点击
        lv_obj_add_event_cb(wish_btn, [](lv_event_t* e) {
            if (s_wish_pending) return;
            lv_obj_t* btn = lv_event_get_target_obj(e);
            auto* url = static_cast<std::string*>(lv_obj_get_user_data(btn));
            if (url == nullptr || url->empty()) return;
            s_wish_pending = true;
            lv_obj_t* lbl = lv_obj_get_child(btn, 0);
            if (lbl) lv_label_set_text(lbl, std::strcmp(Lang::Current(), "zh-CN") == 0 ? "…" : "…");
            valet::sync::ToggleWish(*url, !s_wish, [btn](bool ok, bool new_wish) {
                s_wish_pending = false;
                if (ok) s_wish = new_wish;
                if (lv_obj_is_valid(btn)) {
                    lv_obj_t* lbl = lv_obj_get_child(btn, 0);
                    if (lbl) {
                        const bool zh = std::strcmp(Lang::Current(), "zh-CN") == 0;
                        lv_label_set_text(lbl, s_wish ? (zh ? "✓ 已加入想读" : "✓ In wish list")
                                                      : (zh ? "加到想读" : "Add to wish list"));
                    }
                }
            });
        }, LV_EVENT_CLICKED, nullptr);
        auto* wish_lbl = AddLabel(wish_btn, Text("加到想读", "Add to wish list"), Small());
        lv_obj_center(wish_lbl);
    }
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
                        [](lv_event_t*) {
                            lv_async_call([](void*) {
                                CloseEditor();
                                if (s_opened_from_search && s_stack.size() <= 1) {
                                    OpenBookshelfAsync(nullptr);
                                }
                            }, nullptr);
                        });
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
    auto* hint = AddLabel(panel, Text("同时搜索微信读书与已配置书库，支持书名、作者或拼音首字母。",
                                      "Searches WeRead and every configured library."), Small());
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

// ── 微信读书扫码登录弹窗 ───────────────────────────────────────────────────
struct QrJobMsg {
    uint32_t qr_gen = 0;
    bool has_qr = false;
    std::string qr_url;
    bool done = false;
    bool ok = false;
    std::string status;
};

void CloseWeReadQr() {
    ++s_qr_gen;
    if (s_ui.qr_mask) lv_obj_delete(s_ui.qr_mask);
}

void ApplyWeReadQrMsg(void* p) {
    auto* msg = static_cast<QrJobMsg*>(p);
    if (msg->qr_gen != s_qr_gen || s_ui.qr_mask == nullptr) {
        delete msg;
        return;
    }
    if (msg->has_qr && s_ui.qr_code && !msg->qr_url.empty()) {
        lv_qrcode_update(s_ui.qr_code, msg->qr_url.data(), msg->qr_url.size());
        lv_obj_clear_flag(s_ui.qr_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_ui.qr_mask);
        EpdRequestScreenChange();
    }
    if (s_ui.qr_status && !msg->status.empty()) {
        lv_label_set_text(s_ui.qr_status, msg->status.c_str());
    }
    if (msg->done && msg->ok) {
        CloseWeReadQr();
        s_signature.clear();
        StartRoot();
        ScreenShowToast(Text("微信读书登录成功", "WeRead signed in"));
    }
    delete msg;
}

void WeReadQrWorker(void* arg) {
    const uint32_t my_gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    std::string uid, qr_url, err;
    if (!valet::weread::BeginQrLogin(uid, qr_url, err)) {
        auto* m = new QrJobMsg{my_gen, false, {}, true, false, err.empty() ? "获取二维码失败" : err};
        if (!ScreenLvAsync(ApplyWeReadQrMsg, m)) delete m;
        vTaskDelete(nullptr);
        return;
    }
    {
        auto* m = new QrJobMsg{my_gen, true, qr_url, false, false,
                               Text("请用手机微信扫码确认登录", "Scan with WeChat to sign in")};
        if (!ScreenLvAsync(ApplyWeReadQrMsg, m)) delete m;
    }
    for (int step = 0; step < 90 && my_gen == s_qr_gen; ++step) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (my_gen != s_qr_gen) break;
        valet::weread::QrState st = valet::weread::QrState::kPending;
        std::string poll_err;
        if (!valet::weread::PollQrLogin(uid, {}, st, poll_err)) {
            auto* m = new QrJobMsg{my_gen, false, {}, true, false, poll_err.empty() ? "扫码登录失败" : poll_err};
            if (!ScreenLvAsync(ApplyWeReadQrMsg, m)) delete m;
            vTaskDelete(nullptr);
            return;
        }
        if (st == valet::weread::QrState::kComplete) {
            auto* m = new QrJobMsg{my_gen, false, {}, true, true, Text("登录成功", "Signed in")};
            if (!ScreenLvAsync(ApplyWeReadQrMsg, m)) delete m;
            vTaskDelete(nullptr);
            return;
        }
        if (st == valet::weread::QrState::kExpired) {
            auto* m = new QrJobMsg{my_gen, false, {}, true, false, Text("二维码已过期，请关闭后重试", "QR code expired")};
            if (!ScreenLvAsync(ApplyWeReadQrMsg, m)) delete m;
            vTaskDelete(nullptr);
            return;
        }
    }
    vTaskDelete(nullptr);
}

void OpenWeReadQr() {
    if (s_ui.qr_mask || !s_ui.scr) return;
    ++s_qr_gen;
    const uint32_t my_gen = s_qr_gen;
    auto* mask = ScreenPopupMask(s_ui.scr);
    s_ui.qr_mask = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t*) {
        ++s_qr_gen;
        s_ui.qr_mask = s_ui.qr_code = s_ui.qr_status = s_ui.qr_sub = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ScreenPopupTitleBar(sheet, Text("微信读书扫码登录", "WeRead Sign In"), Strong(), Text("关闭", "Close"), Small(),
                        [](lv_event_t*) { lv_async_call([](void*) { CloseWeReadQr(); }, nullptr); });

    auto* body = lv_obj_create(sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, 33, 0);
    lv_obj_set_style_pad_ver(body, 28, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t qr_side = UiSx(240);
    auto* frame = UiCreateQrFrame(body, qr_side);
    auto* qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(qr, qr_side);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_center(qr);
    lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
    s_ui.qr_code = qr;

    s_ui.qr_status = AddLabel(body, Text("正在获取登录二维码…", "Requesting QR code…"), Book_ItemFont());
    lv_obj_set_width(s_ui.qr_status, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.qr_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.qr_status, LV_LABEL_LONG_WRAP);

    s_ui.qr_sub = AddLabel(body,
                           Text("扫码同步微信读书书架、已购书籍与阅读进度",
                                "Sign in to sync your shelf, purchases and progress."),
                           Small());
    lv_obj_set_width(s_ui.qr_sub, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.qr_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.qr_sub, LV_LABEL_LONG_WRAP);

    if (xTaskCreatePinnedToCore(WeReadQrWorker, "wr_qr", 8192,
                                reinterpret_cast<void*>(static_cast<uintptr_t>(my_gen)),
                                tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        lv_label_set_text(s_ui.qr_status, Text("内存不足，请稍后重试", "Out of memory"));
    }
}

// ── 手机扫码配置书库弹窗（复用 /setup?to=library）─────────────────────────
void CloseSetupQr() {
    if (s_ui.setup_timer) {
        lv_timer_delete(s_ui.setup_timer);
        s_ui.setup_timer = nullptr;
    }
    if (s_ui.setup_mask) {
        lv_obj_delete(s_ui.setup_mask);
    }
    valet::CloseSetupSession();
    const auto libs = valet::LoadLibrarySources();
    const std::string sig = LibrariesSignature(libs);
    if (sig != s_signature && s_ui.scr) {
        s_signature = sig;
        StartRoot();
    }
}

void RefreshSetupQrUi() {
    if (!s_ui.setup_mask) return;
    const auto setup = valet::GetSetupStatus();
    if (setup.running && !setup.url.empty()) {
        std::string lib_url = setup.url;
        const size_t hash = lib_url.find('#');
        if (hash != std::string::npos) {
            lib_url = lib_url.substr(0, hash) + "?to=library" + lib_url.substr(hash);
        }
        if (s_ui.setup_qr && lv_obj_has_flag(s_ui.setup_qr, LV_OBJ_FLAG_HIDDEN)) {
            lv_qrcode_update(s_ui.setup_qr, lib_url.data(), lib_url.size());
            lv_obj_clear_flag(s_ui.setup_qr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_ui.setup_mask);
            EpdRequestScreenChange();
        }
        if (s_ui.setup_status) {
            lv_label_set_text(s_ui.setup_status, Text("同一 Wi-Fi 下用手机扫码打开「书库配置」", "Scan on the same Wi-Fi to configure libraries"));
        }
        if (s_ui.setup_addr && hash != std::string::npos) {
            lv_label_set_text(s_ui.setup_addr, setup.url.substr(0, hash).c_str());
        }
    } else if (setup.starting) {
        if (s_ui.setup_status) {
            lv_label_set_text(s_ui.setup_status, Text("正在连接 Wi-Fi 并开启配置页…", "Starting setup page…"));
        }
    } else if (!setup.note.empty()) {
        if (s_ui.setup_status) {
            lv_label_set_text(s_ui.setup_status, setup.note.c_str());
        }
    }
    // 如果手机在配置页上添加或移除了书库，底层列表自动刷新
    const auto libs = valet::LoadLibrarySources();
    const std::string sig = LibrariesSignature(libs);
    if (sig != s_signature && s_stack.size() <= 1) {
        s_signature = sig;
        StartRoot();
    }
}

void OpenSetupQr() {
    if (s_ui.setup_mask || !s_ui.scr) return;
    auto* mask = ScreenPopupMask(s_ui.scr);
    s_ui.setup_mask = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t*) {
        if (s_ui.setup_timer) {
            lv_timer_delete(s_ui.setup_timer);
            s_ui.setup_timer = nullptr;
        }
        s_ui.setup_mask = s_ui.setup_qr = s_ui.setup_status = s_ui.setup_addr = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    auto* sheet = lv_obj_create(mask);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    ScreenPopupTitleBar(sheet, Text("添加 / 管理书库", "Manage Libraries"), Strong(), Text("完成", "Done"), Small(),
                        [](lv_event_t*) { lv_async_call([](void*) { CloseSetupQr(); }, nullptr); });

    auto* body = lv_obj_create(sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(body, 33, 0);
    lv_obj_set_style_pad_ver(body, 28, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    const lv_coord_t qr_side = UiSx(240);
    auto* frame = UiCreateQrFrame(body, qr_side);
    auto* qr = lv_qrcode_create(frame);
    lv_qrcode_set_size(qr, qr_side);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_center(qr);
    lv_obj_add_flag(qr, LV_OBJ_FLAG_HIDDEN);
    s_ui.setup_qr = qr;

    s_ui.setup_status = AddLabel(body, Text("正在连接 Wi-Fi 并开启配置页…", "Starting setup page…"), Book_ItemFont());
    lv_obj_set_width(s_ui.setup_status, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.setup_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.setup_status, LV_LABEL_LONG_WRAP);

    s_ui.setup_addr = AddLabel(body, Text("支持粘贴 OPDS 公网地址与认证口令", "Paste OPDS URL and credentials on your phone"), Small());
    lv_obj_set_width(s_ui.setup_addr, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.setup_addr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_ui.setup_addr, LV_LABEL_LONG_WRAP);

    s_ui.setup_timer = lv_timer_create([](lv_timer_t*) { RefreshSetupQrUi(); }, 600, nullptr);
    Application::GetInstance().Schedule([]() {
        valet::StartSetupServer();
        ScreenLvAsync([](void*) { RefreshSetupQrUi(); }, nullptr);
    }, Application::TaskDomain::Network);
}

// ── 按键 ──────────────────────────────────────────────────────────────────
void GoBack() {
    if (s_ui.setup_mask) { CloseSetupQr(); return; }
    if (s_ui.qr_mask) { CloseWeReadQr(); return; }
    if (s_ui.detail) { CloseDetail(); return; }
    if (s_ui.editor) {
        CloseEditor();
        if (s_opened_from_search && s_stack.size() <= 1) {
            s_gate.RequestStop();
            lv_async_call(OpenBookshelfAsync, nullptr);
        }
        return;
    }
    Level* top = Top();
    if (top && top->page > 0) { --top->page; Render(); return; }
    if (s_stack.size() > 1) {
        s_stack.pop_back();
        ++s_gen;
        s_loading = false;
        s_error.clear();
        if (s_opened_from_search && s_stack.size() == 1) {
            s_gate.RequestStop();
            lv_async_call(OpenBookshelfAsync, nullptr);
            return;
        }
        if (Top()->entries.empty() && !Top()->url.empty()) {
            if (Top()->url == "weread://shelf") FetchLevel(JobKind::kWeReadShelf, Top()->url);
            else FetchLevel(JobKind::kBrowse, Top()->url);
        } else {
            Render();
        }
        return;
    }
    s_gate.RequestStop();
    lv_async_call(OpenBookshelfAsync, nullptr);
}

void GoNext() {
    if (s_ui.setup_mask || s_ui.qr_mask || s_ui.detail || s_ui.editor || s_loading) return;
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
    // 书库配置页：管理微信读书账号与各 OPDS 书库订阅
    Level level;
    level.title = Text("书库", "Libraries");
    {
        valet::opds::Entry wr;
        if (valet::weread::IsLoggedIn()) {
            wr.title = Text("微信读书 · 我的书架", "WeRead · My Shelf");
            wr.author = Text("已登录 · 同步云端书架、已购书籍与阅读进度", "Signed in · Syncs shelf, purchases & progress");
            wr.nav = "weread://shelf";
        } else {
            wr.title = Text("微信读书 · 扫码登录", "WeRead · Sign In");
            wr.author = Text("未登录 · 扫码同步微信读书书架与已购书籍",
                             "Sign in to sync your WeRead shelf & purchases");
            wr.nav = "weread://login";
        }
        level.entries.push_back(std::move(wr));
    }
    for (const auto& lib : libs) {
        valet::opds::Entry entry;
        entry.title = lib.name;
        std::string host = valet::LibraryNameFromUrl(lib.url);
        std::string desc = host.empty() ? lib.url : host;
        if (lib.rich) desc += Text(" · 进度同步", " · Syncs progress");
        else desc += " · OPDS";
        if (!lib.password.empty() || !lib.user.empty() || lib.rich) {
            desc += Text(" · 已认证", " · Authenticated");
        }
        entry.author = desc;
        entry.nav = lib.url;
        level.entries.push_back(std::move(entry));
    }
    {
        valet::opds::Entry add;
        add.title = Text("+ 添加 / 管理书库订阅", "+ Add or Manage Libraries");
        add.author = Text("手机扫码粘贴 OPDS 公网地址与认证口令", "Scan with phone to add OPDS URL & password");
        add.nav = "setup://library";
        level.entries.push_back(std::move(add));
    }
    s_stack.push_back(std::move(level));
    s_loading = false;
    Render();
}
} // namespace

bool OpdsHasLibraries() { return true; }

void OnShelfSearchClicked(lv_event_t*) {
    s_open_search_immediately = true;
    lv_async_call([](void*) { ScreenLoadReplace(CreateOpdsScreen()); }, nullptr);
}

void OnShelfLibraryClicked(lv_event_t*) {
    s_open_search_immediately = false;
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
        if (s_ui.setup_timer) {
            lv_timer_delete(s_ui.setup_timer);
            s_ui.setup_timer = nullptr;
        }
        valet::CloseSetupSession();
        s_ui = Ui{};
        ++s_gen;
        ++s_qr_gen;
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
    s_ui.head_btn = search;
    lv_obj_remove_style_all(search);
    lv_obj_set_size(search, LV_SIZE_CONTENT, kHeadH);
    lv_obj_set_style_pad_hor(search, UiSx(10), 0);
    lv_obj_set_style_border_width(search, 2, 0);
    lv_obj_set_style_border_color(search, lv_color_black(), 0);
    lv_obj_set_style_radius(search, 4, 0);
    lv_obj_add_flag(search, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(search);
    lv_obj_add_event_cb(search, [](lv_event_t*) { if (!s_loading) OpenEditor(); }, LV_EVENT_CLICKED, nullptr);
    auto* search_text = AddLabel(search, Text("换个词", "Search"), Book_ItemFont());
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
    if (s_open_search_immediately) {
        s_open_search_immediately = false;
        s_opened_from_search = true;
        s_signature = sig;
        StartRoot();
        OpenEditor();
    } else {
        s_opened_from_search = false;
        s_signature = sig;
        StartRoot();
    }
    return scr;
}
