#include "reader_sync.h"

#include "assets/lang_config.h"
#include "board.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "http.h"
#include "power_policy.h"
#include "reader/book_storage_paths.h"
#include "reader/image_util.h"
#include "screen_common.h"
#include "weread_client.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

namespace valet::sync {
namespace {
constexpr const char* TAG = "ReaderSync";
constexpr uint32_t kTaskStack = 8 * 1024;
constexpr int kJumpMinX10 = 5;   // 别处的进度和本机差不到 0.5% 就不跳

// 每本书一份：进度地址 + 上次看到的服务器 updated。
// 存在书的辅助数据旁边（.sync），第一行地址，第二行 seen；删书时跟着书的辅助数据走。
struct Book {
    std::string url;      // 进度地址（含短码，不进日志）
    long long seen = 0;   // 上次看到的服务器 updated（秒）
    int pushed_x10 = -1;  // 这次开机里上次写回的进度（只在内存）
    bool loaded = false;  // 已经读过 .sync
};

std::mutex s_mu;
std::map<std::string, Book> s_books;
std::map<std::string, std::string> s_hints;   // 书库页给的地址，打开时用一次

std::string SyncPath(const std::string& book_path) {
    return reader::BookStoragePath(book_path.c_str(), ".sync");
}

// 在工作任务里调用（碰 SD）
void ReadSidecar(const std::string& book_path, Book& book) {
    const auto path = SyncPath(book_path);
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return;
    char buf[320] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    char* newline = std::strchr(buf, '\n');
    if (newline != nullptr) {
        *newline = '\0';
        book.seen = std::strtoll(newline + 1, nullptr, 10);
    }
    book.url = buf;
}

void WriteSidecar(const std::string& book_path, const std::string& url, long long seen) {
    const auto path = SyncPath(book_path);
    if (path.empty() || url.empty()) return;
    const std::string text = url + "\n" + std::to_string(seen) + "\n";
    if (!reader::WriteFileAtomic(path.c_str(), reinterpret_cast<const uint8_t*>(text.data()), text.size()))
        ESP_LOGW(TAG, "sidecar write failed");
}

// 书号只用于日志：地址里 /books/<id>/progress 的 <id>
std::string BookIdForLog(const std::string& url) {
    const size_t end = url.rfind("/progress");
    if (end == std::string::npos || end == 0) return "?";
    const size_t start = url.rfind('/', end - 1);
    return start == std::string::npos ? "?" : url.substr(start + 1, end - start - 1);
}

bool ClockValid(long long& now) {
    const time_t t = time(nullptr);
    struct tm tm_info = {};
    if (t == static_cast<time_t>(-1) || localtime_r(&t, &tm_info) == nullptr || tm_info.tm_year < (2025 - 1900)) return false;
    now = static_cast<long long>(t);
    return true;
}

// 一次 HTTP；返回状态码，0 = 没连上
int Request(const char* method, const std::string& url, std::string body, std::string& response) {
    PowerNeedHold hold(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady(8000)) return 0;
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) return 0;
    auto http = network->CreateHttp(0);
    if (http == nullptr) return 0;
    http->SetTimeout(10000);
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Connection", "close");
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (!http->Open(method, url)) return 0;
    const int status = http->GetStatusCode();
    response = http->ReadAll();
    http->Close();
    return status;
}

bool StartTask(TaskFunction_t fn, const char* name, void* arg) {
    return xTaskCreatePinnedToCoreWithCaps(fn, name, kTaskStack, arg, tskIDLE_PRIORITY + 1, nullptr, 0,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS;
}

// 记一个更新的 seen，并写回 .sync（工作任务里调用）
void NoteSeen(const std::string& book_path, long long updated) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_books.find(book_path);
        if (it == s_books.end() || updated <= it->second.seen) return;
        it->second.seen = updated;
        url = it->second.url;
    }
    WriteSidecar(book_path, url, updated);
}

// ── 打开时拉进度 ─────────────────────────────────────────────────────────
struct PullJob { std::string path; std::string hint; int local_x10; };
struct PullDone { std::string path; int x10; int spine; bool from_weread; };

void ApplyPull(void* p) {
    auto* done = static_cast<PullDone*>(p);
    auto& st = Book_State();
    if (st.reader.session && st.reader.session->IsOpen() && st.reader.session->Info().path == done->path &&
        st.reader.read_chrome == BookUiState::ReadChrome::kReading) {
        bool jumped = false;
        if (done->spine >= 0 && done->spine != st.reader.session->SourceChapterIndex()) {
            jumped = RequestReaderNavigation(reader::BookSession::NavigationKind::kToc, done->spine);
        } else if (done->x10 >= 0) {
            jumped = RequestReaderNavigation(reader::BookSession::NavigationKind::kProgress, done->x10);
        }
        if (jumped) {
            char text[96];
            if (done->from_weread) {
                std::snprintf(text, sizeof(text), "%s %d%%",
                              std::strcmp(Lang::Current(), "zh-CN") == 0 ? "已同步微信读书进度" : "Synced WeRead progress",
                              (std::max(0, done->x10) + 5) / 10);
            } else {
                std::snprintf(text, sizeof(text), "%s %d%%",
                              std::strcmp(Lang::Current(), "zh-CN") == 0 ? "接着别处读到的地方" : "Continued from elsewhere",
                              (std::max(0, done->x10) + 5) / 10);
            }
            ScreenShowToast(text);
        }
    }
    delete done;
}

void PullTask(void* arg) {
    auto* job = static_cast<PullJob*>(arg);
    // 先把这本书的同步记录准备好：内存里没有就读 .sync，没有 .sync 就用书库页给的地址
    std::string url;
    long long seen = 0;
    bool write_hint = false;
    {
        Book fresh;
        bool need_read = false;
        {
            std::lock_guard<std::mutex> lock(s_mu);
            need_read = !s_books[job->path].loaded;
        }
        if (need_read) ReadSidecar(job->path, fresh);
        std::lock_guard<std::mutex> lock(s_mu);
        auto& book = s_books[job->path];
        if (!book.loaded) {
            book.loaded = true;
            if (book.url.empty()) { book.url = fresh.url; book.seen = fresh.seen; }
        }
        if (book.url.empty() && !job->hint.empty()) { book.url = job->hint; write_hint = true; }
        book.pushed_x10 = job->local_x10;   // 打开时的位置不算「读过」，不用写回
        url = book.url;
        seen = book.seen;
    }
    if (write_hint) WriteSidecar(job->path, url, seen);
    if (url.empty()) { delete job; vTaskDeleteWithCaps(nullptr); return; }

    if (url.rfind("weread://", 0) == 0) {
        const std::string wr_id = url.substr(9);
        valet::weread::RemoteProgress wr_prog;
        std::string wr_err;
        if (valet::weread::FetchProgress(wr_id, wr_prog, wr_err)) {
            const int spine = valet::weread::SpineForChapterUid(job->path, wr_prog.chapter_uid);
            const long long updated = wr_prog.update_time;
            const int remote_x10 = wr_prog.progress_x10;
            ESP_LOGI(TAG, "pull weread book=%s remote=%d local=%d ch=%s spine=%d updated=%lld seen=%lld",
                     wr_id.c_str(), remote_x10, job->local_x10, wr_prog.chapter_uid.c_str(), spine, updated, seen);
            if (updated > seen && (std::abs(remote_x10 - job->local_x10) >= kJumpMinX10 || spine >= 0)) {
                auto* done = new PullDone{job->path, remote_x10, spine, true};
                if (!ScreenLvAsync(ApplyPull, done)) delete done;
            }
            if (updated > seen) NoteSeen(job->path, updated);
        }
        delete job;
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    std::string response;
    const int status = Request("GET", url, {}, response);
    long long updated = 0;
    int remote_x10 = -1;
    if (status == 200) {
        if (cJSON* root = cJSON_Parse(response.c_str())) {
            const cJSON* percent = cJSON_GetObjectItemCaseSensitive(root, "percent");
            const cJSON* when = cJSON_GetObjectItemCaseSensitive(root, "updated");
            if (cJSON_IsNumber(percent))
                remote_x10 = static_cast<int>(std::lround(std::clamp(percent->valuedouble, 0.0, 1.0) * 1000));
            if (cJSON_IsNumber(when)) updated = static_cast<long long>(when->valuedouble);
            cJSON_Delete(root);
        }
    }
    ESP_LOGI(TAG, "pull book=%s status=%d remote=%d local=%d updated=%lld seen=%lld", BookIdForLog(url).c_str(), status,
             remote_x10, job->local_x10, updated, seen);
    // 只在「上次同步以后别处又写过」时跳：本机自己写成功的那次 updated 已经记成 seen
    if (remote_x10 >= 0 && updated > seen && std::abs(remote_x10 - job->local_x10) >= kJumpMinX10) {
        auto* done = new PullDone{job->path, remote_x10, -1, false};
        if (!ScreenLvAsync(ApplyPull, done)) delete done;
    }
    if (updated > seen) NoteSeen(job->path, updated);
    delete job;
    vTaskDeleteWithCaps(nullptr);
}

// ── 写回 ──────────────────────────────────────────────────────────────────
struct PushJob { std::string path; std::string url; Position pos; };
std::mutex s_push_mu;
bool s_push_running = false;
std::map<std::string, PushJob> s_push_pending;   // 每本只留最新的一次

void PushTask(void*) {
    while (true) {
        PushJob job;
        {
            std::lock_guard<std::mutex> lock(s_push_mu);
            if (s_push_pending.empty()) { s_push_running = false; break; }
            job = std::move(s_push_pending.begin()->second);
            s_push_pending.erase(s_push_pending.begin());
        }
        if (job.url.rfind("weread://", 0) == 0) {
            const std::string wr_id = job.url.substr(9);
            const std::string ch_uid = valet::weread::ChapterUidForSpine(job.path, job.pos.spine);
            std::string wr_err;
            const bool ok = valet::weread::UploadProgress(
                wr_id, ch_uid, std::max(1, job.pos.spine + 1),
                std::max(0, job.pos.spine_x1000), job.pos.x10, 30, wr_err);
            ESP_LOGI(TAG, "push weread book=%s x10=%d spine=%d ch=%s ok=%d err=[%s]",
                     wr_id.c_str(), job.pos.x10, job.pos.spine, ch_uid.c_str(), ok ? 1 : 0, wr_err.c_str());
            if (!ok) {
                std::lock_guard<std::mutex> lock(s_mu);
                auto it = s_books.find(job.path);
                if (it != s_books.end() && it->second.pushed_x10 == job.pos.x10) it->second.pushed_x10 = -1;
                continue;
            }
            long long now = 0;
            if (ClockValid(now)) NoteSeen(job.path, now);
            continue;
        }
        cJSON* body = cJSON_CreateObject();
        if (body == nullptr) continue;
        cJSON_AddNumberToObject(body, "percent", job.pos.x10 / 1000.0);
        if (job.pos.spine >= 0) {
            cJSON_AddNumberToObject(body, "spine", job.pos.spine);
            cJSON_AddNumberToObject(body, "spine_percent", job.pos.spine_x1000 / 1000.0);
        }
        cJSON_AddStringToObject(body, "format", job.pos.format);
        cJSON_AddStringToObject(body, "device", "valet-e-ink");
        long long now = 0;
        if (ClockValid(now)) cJSON_AddNumberToObject(body, "client_updated", static_cast<double>(now));
        char* text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (text == nullptr) continue;
        std::string response;
        const int status = Request("POST", job.url, text, response);
        cJSON_free(text);
        long long updated = 0;
        bool kept_newer = false;
        if (status >= 200 && status < 300) {
            if (cJSON* root = cJSON_Parse(response.c_str())) {
                const cJSON* when = cJSON_GetObjectItemCaseSensitive(root, "updated");
                const cJSON* kept = cJSON_GetObjectItemCaseSensitive(root, "kept");
                if (cJSON_IsNumber(when)) updated = static_cast<long long>(when->valuedouble);
                kept_newer = cJSON_IsString(kept) && kept->valuestring && std::strcmp(kept->valuestring, "newer") == 0;
                cJSON_Delete(root);
            }
        }
        ESP_LOGI(TAG, "push book=%s x10=%d spine=%d status=%d updated=%lld%s", BookIdForLog(job.url).c_str(),
                 job.pos.x10, job.pos.spine, status, updated, kept_newer ? " kept=newer" : "");
        if (status < 200 || status >= 300) {
            // 没写成：下次检查点再写
            std::lock_guard<std::mutex> lock(s_mu);
            auto it = s_books.find(job.path);
            if (it != s_books.end() && it->second.pushed_x10 == job.pos.x10) it->second.pushed_x10 = -1;
            continue;
        }
        // 自己写成的这次记成已看过，下次打开不会把自己的进度当成「别处读的」
        if (updated > 0 && !kept_newer) NoteSeen(job.path, updated);
    }
    vTaskDeleteWithCaps(nullptr);
}

struct SaveJob { std::string path; std::string url; };
void SaveTask(void* arg) {
    auto* job = static_cast<SaveJob*>(arg);
    WriteSidecar(job->path, job->url, 0);
    delete job;
    vTaskDeleteWithCaps(nullptr);
}

// ── 想读 ──────────────────────────────────────────────────────────────────
// 从 progress URL 推出 wish URL：…/progress → …/wish
static std::string WishUrl(const std::string& progress_url) {
    const std::string suffix = "/progress";
    if (progress_url.size() <= suffix.size()) return {};
    if (progress_url.compare(progress_url.size() - suffix.size(), suffix.size(), suffix) != 0) return {};
    return progress_url.substr(0, progress_url.size() - suffix.size()) + "/wish";
}

struct WishJob {
    std::string progress_url;
    bool on;
    std::function<void(bool, bool)> callback;
};

void WishTask(void* arg) {
    auto* job = static_cast<WishJob*>(arg);
    const std::string url = WishUrl(job->progress_url);
    cJSON* body = cJSON_CreateObject();
    bool ok = false;
    bool new_wish = job->on;
    if (body != nullptr && !url.empty()) {
        cJSON_AddBoolToObject(body, "on", job->on ? cJSON_True : cJSON_False);
        char* text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (text != nullptr) {
            std::string response;
            const int status = Request("POST", url, text, response);
            cJSON_free(text);
            ok = (status >= 200 && status < 300);
            if (ok) {
                if (cJSON* root = cJSON_Parse(response.c_str())) {
                    const cJSON* w = cJSON_GetObjectItemCaseSensitive(root, "wish");
                    if (cJSON_IsBool(w)) new_wish = cJSON_IsTrue(w);
                    cJSON_Delete(root);
                }
            }
            ESP_LOGI(TAG, "wish book=%s on=%d status=%d", BookIdForLog(job->progress_url).c_str(), job->on ? 1 : 0, status);
        }
    } else {
        if (body) cJSON_Delete(body);
    }
    // 回调要在 LVGL 任务执行
    struct Cb { std::function<void(bool,bool)> fn; bool ok; bool wish; };
    auto* cb = new Cb{std::move(job->callback), ok, new_wish};
    if (!ScreenLvAsync([](void* p) {
            auto* c = static_cast<Cb*>(p);
            c->fn(c->ok, c->wish);
            delete c;
        }, cb)) delete cb;
    delete job;
    vTaskDeleteWithCaps(nullptr);
}
} // namespace

std::string ProgressUrlFromDownload(const std::string& download_url) {
    // http(s)://host/opds/<短码>/download/<id>/<格式>/ → http(s)://host/api/u/<短码>/books/<id>/progress
    const size_t scheme = download_url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t path = download_url.find('/', scheme + 3);
    if (path == std::string::npos) return {};
    const std::string base = download_url.substr(0, path);
    const std::string rest = download_url.substr(path);
    const std::string prefix = "/opds/";
    if (rest.compare(0, prefix.size(), prefix) != 0) return {};
    const size_t key_end = rest.find('/', prefix.size());
    if (key_end == std::string::npos) return {};
    const std::string key = rest.substr(prefix.size(), key_end - prefix.size());
    if (key.empty() || key == "download") return {};   // 没有短码的 Calibre-Web 原地址
    for (char c : key) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return {};
    }
    const std::string marker = "/download/";
    if (rest.compare(key_end, marker.size(), marker) != 0) return {};
    const size_t id_start = key_end + marker.size();
    size_t id_end = id_start;
    while (id_end < rest.size() && std::isdigit(static_cast<unsigned char>(rest[id_end]))) ++id_end;
    if (id_end == id_start) return {};
    return base + "/api/u/" + key + "/books/" + rest.substr(id_start, id_end - id_start) + "/progress";
}

void Remember(const std::string& book_path, const std::string& progress_url) {
    if (book_path.empty() || progress_url.empty()) return;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        auto& book = s_books[book_path];
        if (book.url == progress_url) return;
        book.url = progress_url;
        book.seen = 0;
        book.loaded = true;   // 新下的书：内存里这份就是最新的
    }
    ESP_LOGI(TAG, "remember book=%s", BookIdForLog(progress_url).c_str());
    auto* job = new SaveJob{book_path, progress_url};
    if (!StartTask(SaveTask, "sync_save", job)) delete job;
}

void HintBeforeOpen(const std::string& book_path, const std::string& progress_url) {
    if (book_path.empty() || progress_url.empty()) return;
    std::lock_guard<std::mutex> lock(s_mu);
    s_hints[book_path] = progress_url;
}

void PullOnOpen(const std::string& book_path, int local_x10) {
    if (book_path.empty()) return;
    std::string hint;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_hints.find(book_path);
        if (it != s_hints.end()) { hint = std::move(it->second); s_hints.erase(it); }
    }
    auto* job = new PullJob{book_path, hint, local_x10};
    if (!StartTask(PullTask, "sync_pull", job)) delete job;
}

void Push(const std::string& book_path, const Position& pos) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        auto it = s_books.find(book_path);
        if (it == s_books.end() || it->second.url.empty() || it->second.pushed_x10 == pos.x10) return;
        it->second.pushed_x10 = pos.x10;
        url = it->second.url;
    }
    std::lock_guard<std::mutex> lock(s_push_mu);
    s_push_pending[book_path] = PushJob{book_path, url, pos};
    if (s_push_running) return;
    s_push_running = true;
    if (!StartTask(PushTask, "sync_push", nullptr)) s_push_running = false;
}

void ToggleWish(const std::string& progress_url, bool on,
                std::function<void(bool ok, bool new_wish)> callback) {
    if (progress_url.empty()) { callback(false, on); return; }
    auto* job = new WishJob{progress_url, on, std::move(callback)};
    if (!StartTask(WishTask, "sync_wish", job)) {
        job->callback(false, on);
        delete job;
    }
}

} // namespace valet::sync
