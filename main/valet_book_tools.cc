#include "valet_book_tools.h"
#include "valet_book_search.h"

#include "reader/ebook_document.h"
#include "reader/epub_document.h"
#include "reader/reader_types.h"
#include "reader/text_encoding.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#define TAG "BookTools"

namespace valet {
namespace bs = booksearch;
namespace {
std::mutex source_mutex;
BookSource latest_source;
std::atomic<bool> busy{false};
constexpr int64_t kSearchBudgetUs = 25 * 1000000;   // 网关那边等 45 秒；搜不完先回已找到的，并说从哪一章接着搜
constexpr uint32_t kStack = 20 * 1024;

// 一本书按章节流式读出正文段（只要文字）。
class Book {
public:
    bool Open(const BookSource& source) {
        format_ = static_cast<reader::BookFormat>(source.format);
        path_ = source.path;
        if (format_ == reader::BookFormat::kEpub) return epub_.Open(path_.c_str(), true);
        if (format_ == reader::BookFormat::kEbook) return ebook_.Open(path_.c_str());
        return format_ == reader::BookFormat::kTxt;
    }
    int Chapters() const {
        if (format_ == reader::BookFormat::kEpub) return epub_.SpineCount();
        if (format_ == reader::BookFormat::kEbook) return ebook_.ChapterCount();
        return 1;
    }
    std::string Title(int chapter) const {
        if (format_ == reader::BookFormat::kEbook) return ebook_.ChapterTitle(chapter);
        if (format_ == reader::BookFormat::kEpub) {
            std::string title;
            for (const auto& entry : epub_.Toc())
                if (entry.spine_index >= 0 && entry.spine_index <= chapter && !entry.title.empty()) title = entry.title;
            return title;
        }
        return {};
    }
    // 每一块交给 sink(text, continuation)；sink 返回 false 停下。
    bool Stream(int chapter, const std::function<bool(const std::string&, bool)>& sink) {
        const auto blocks = [&](reader::ContentBlock&& block) {
            if (block.kind != reader::ContentKind::kText || block.text.empty()) return true;
            return sink(block.text, block.continuation);
        };
        if (format_ == reader::BookFormat::kEpub) return epub_.StreamChapterBlocks(chapter, blocks);
        if (format_ == reader::BookFormat::kEbook) return ebook_.StreamChapterBlocks(chapter, blocks);
        return StreamTxt(sink);
    }

private:
    // TXT：一行一段，空行跳过；开头 4 KB 不像 UTF-8 就按 GB18030 转。
    bool StreamTxt(const std::function<bool(const std::string&, bool)>& sink) {
        std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path_.c_str(), "rb"), std::fclose);
        if (!file) return false;
        std::vector<uint8_t> chunk(4096);
        size_t n = std::fread(chunk.data(), 1, chunk.size(), file.get());
        const bool utf8 = reader::LooksLikeUtf8(chunk.data(), n);
        std::string line, text;
        const auto emit = [&]() {
            size_t a = 0, b = line.size();
            while (a < b && (line[a] == ' ' || line[a] == '\t' || line[a] == '\r')) ++a;
            while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == '\r')) --b;
            bool keep = true;
            if (b > a) {
                text.clear();
                if (utf8) text.assign(line, a, b - a);
                else reader::BytesToUtf8(reinterpret_cast<const uint8_t*>(line.data() + a), b - a, true, text);
                if (!text.empty()) keep = sink(text, false);
            }
            line.clear();
            return keep;
        };
        while (n > 0) {
            for (size_t i = 0; i < n; ++i) {
                if (chunk[i] == '\n') { if (!emit()) return true; }
                else if (line.size() < 64 * 1024) line.push_back(static_cast<char>(chunk[i]));
            }
            n = std::fread(chunk.data(), 1, chunk.size(), file.get());
        }
        emit();
        return true;
    }
    reader::BookFormat format_ = reader::BookFormat::kUnknown;
    std::string path_;
    reader::EpubDocument epub_;
    reader::EbookDocument ebook_;
};

cJSON* Error(const char* message) {
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return root;
}
std::string Str(const cJSON* object, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
int Int(const cJSON* object, const char* key, int fallback, int lo, int hi) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return fallback;
    const int v = item->valueint;
    return v < lo ? lo : v > hi ? hi : v;
}

// 这一章里「当前页」是第几段；找不到返回 -1。
int FindAnchor(Book& book, int chapter, const std::string& anchor) {
    if (anchor.empty()) return -1;
    int found = -1;
    bs::ParagraphJoiner joiner;
    const auto check = [&]() { if (found < 0 && bs::ParagraphHasAnchor(joiner.Finished(), anchor)) found = joiner.FinishedIndex(); };
    book.Stream(chapter, [&](const std::string& text, bool cont) {
        if (joiner.Feed(text, cont)) check();
        return found < 0;
    });
    if (found < 0 && joiner.Flush()) check();
    return found;
}

cJSON* Search(Book& book, const BookSource& source, const cJSON* request) {
    const std::string query = bs::NormalizeQuery(Str(request, "query"));
    if (query.empty()) return Error("query must be 1-120 bytes");
    const int limit = Int(request, "limit", bs::kDefaultHits, 1, bs::kMaxHits);
    const bool read_only = Str(request, "scope") == "read";
    const int chapters = book.Chapters();
    const int from = Int(request, "fromChapter", 0, 0, chapters);
    // 只搜读过的：到当前这一章为止，这一章只到当前页往后几段（一页大约就这么多）
    const int last = read_only && source.chapter >= 0 ? std::min(source.chapter, chapters - 1) : chapters - 1;
    const int64_t deadline = esp_timer_get_time() + kSearchBudgetUs;
    auto* root = cJSON_CreateObject();
    auto* hits = cJSON_CreateArray();
    size_t bytes = 0;
    int count = 0, current = -1, next_chapter = -1;
    bool more = false;
    for (int chapter = from; chapter <= last && !more; ++chapter) {
        if (esp_timer_get_time() > deadline) { next_chapter = chapter; break; }
        const std::string title = book.Title(chapter);
        const bool here = chapter == source.chapter;
        int stop_after = -1;
        bs::ParagraphJoiner joiner;
        const auto check = [&]() {
            const auto& text = joiner.Finished();
            const int index = joiner.FinishedIndex();
            if (here && current < 0 && bs::ParagraphHasAnchor(text, source.anchor)) {
                current = index;
                if (read_only) stop_after = index + 6;
            }
            if (stop_after >= 0 && index > stop_after) return false;
            const size_t at = bs::Find(text, query);
            if (at == std::string::npos) return true;
            if (count >= limit) { more = true; return false; }
            std::string excerpt = bs::Excerpt(text, at, query.size());
            if (bytes + excerpt.size() > bs::kSearchReplyMax) { more = true; return false; }
            bytes += excerpt.size();
            auto* hit = cJSON_CreateObject();
            cJSON_AddStringToObject(hit, "at", bs::FormatPosition(chapter, index).c_str());
            if (!title.empty()) cJSON_AddStringToObject(hit, "chapter", bs::ClipUtf8(title, 120).c_str());
            cJSON_AddStringToObject(hit, "text", excerpt.c_str());
            cJSON_AddItemToArray(hits, hit);
            ++count;
            return true;
        };
        book.Stream(chapter, [&](const std::string& text, bool cont) {
            if (joiner.Feed(text, cont) && !check()) return false;
            return esp_timer_get_time() <= deadline;
        });
        if (!more && joiner.Flush()) check();
        // 凑满了条数：从这一章再搜一遍能接上（这一章已给出的命中会重复，但不会漏）
        if (more) { next_chapter = chapter + (count == 0 ? 1 : 0); break; }
        if (esp_timer_get_time() > deadline && !more && chapter < last) { next_chapter = chapter + 1; break; }
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "scope", read_only ? "read" : "all");
    cJSON_AddItemToObject(root, "hits", hits);
    cJSON_AddBoolToObject(root, "more", more);
    cJSON_AddBoolToObject(root, "complete", next_chapter < 0);
    if (next_chapter >= 0) cJSON_AddNumberToObject(root, "nextChapter", next_chapter);
    cJSON_AddNumberToObject(root, "chapters", chapters);
    if (current >= 0) cJSON_AddStringToObject(root, "current", bs::FormatPosition(source.chapter, current).c_str());
    ESP_LOGI(TAG, "search: %d hit(s), more=%d, chapters %d..%d of %d%s", count, more, from, next_chapter < 0 ? last : next_chapter - 1,
             chapters, next_chapter >= 0 ? " (time budget reached)" : "");
    return root;
}

cJSON* Read(Book& book, const BookSource& source, const cJSON* request) {
    const std::string at = Str(request, "at");
    const int chapters = book.Chapters();
    bs::Position pos;
    if (at.empty() || at == "current") {
        if (source.chapter < 0 || source.chapter >= chapters) return Error("the reading position is unknown");
        pos.chapter = source.chapter;
        pos.paragraph = std::max(0, FindAnchor(book, source.chapter, source.anchor));
    } else {
        pos = bs::ParsePosition(at);
        if (!pos.Valid() || pos.chapter >= chapters) return Error("at must look like c15.p32 (from a search result) or \"current\"");
    }
    const int before = Int(request, "before", 2, 0, bs::kMaxBefore);
    const int after = Int(request, "after", 12, 0, bs::kMaxAfter);
    const int first = std::max(0, pos.paragraph - before), last = pos.paragraph + after;
    std::vector<std::pair<int, std::string>> got;
    bool past_end = false;   // 这一章在 last 之后还有段
    bs::ParagraphJoiner joiner;
    const auto take = [&]() {
        const int index = joiner.FinishedIndex();
        if (index > last) { past_end = true; return false; }
        if (index >= first) got.emplace_back(index, bs::ClipUtf8(joiner.Finished(), bs::kReadReplyMax));
        return true;
    };
    if (!book.Stream(pos.chapter, [&](const std::string& text, bool cont) { return !joiner.Feed(text, cont) || take(); }) && got.empty() && !past_end)
        return Error("could not read that chapter");
    if (!past_end && joiner.Flush()) take();
    if (got.empty()) return Error("that position is past the end of the chapter");
    // 合计超过上限：从离目标段最远的一头开始丢
    size_t total = 0;
    for (const auto& p : got) total += p.second.size();
    bool cut_front = false, cut_back = false;
    while (total > bs::kReadReplyMax && got.size() > 1) {
        const bool drop_front = pos.paragraph - got.front().first > got.back().first - pos.paragraph;
        auto& victim = drop_front ? got.front() : got.back();
        total -= victim.second.size();
        if (drop_front) { got.erase(got.begin()); cut_front = true; } else { got.pop_back(); cut_back = true; }
    }
    auto* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    const std::string title = book.Title(pos.chapter);
    if (!title.empty()) cJSON_AddStringToObject(root, "chapter", bs::ClipUtf8(title, 120).c_str());
    if (pos.chapter == source.chapter) cJSON_AddStringToObject(root, "note", "this chapter is where the reader is now");
    auto* list = cJSON_CreateArray();
    for (const auto& p : got) {
        auto* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "at", bs::FormatPosition(pos.chapter, p.first).c_str());
        cJSON_AddStringToObject(item, "text", p.second.c_str());
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddItemToObject(root, "paragraphs", list);
    if (got.front().first > 0) cJSON_AddStringToObject(root, "prev", bs::FormatPosition(pos.chapter, got.front().first - 1).c_str());
    else if (pos.chapter > 0) cJSON_AddStringToObject(root, "prevChapter", bs::FormatPosition(pos.chapter - 1, 0).c_str());
    if (past_end || cut_back) cJSON_AddStringToObject(root, "next", bs::FormatPosition(pos.chapter, got.back().first + 1).c_str());
    else if (pos.chapter + 1 < chapters) cJSON_AddStringToObject(root, "next", bs::FormatPosition(pos.chapter + 1, 0).c_str());
    (void)cut_front;
    ESP_LOGI(TAG, "read: c%d paragraphs %d..%d (%u bytes)", pos.chapter, got.front().first, got.back().first, unsigned(total));
    return root;
}

struct Job {
    BookSource source;
    cJSON* request = nullptr;
    std::function<void(cJSON*)> reply;
};

void Run(void* arg) {
    std::unique_ptr<Job> job(static_cast<Job*>(arg));
    const int64_t started = esp_timer_get_time();
    cJSON* result = nullptr;
    {
        Book book;
        const std::string op = Str(job->request, "op");
        if (!book.Open(job->source)) result = Error("could not open the book on the device");
        else if (op == "search") result = Search(book, job->source, job->request);
        else if (op == "read") result = Read(book, job->source, job->request);
        else result = Error("unknown op");
    }
    cJSON_Delete(job->request);
    ESP_LOGI(TAG, "%s done in %d ms", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "ok")) ? "request" : "request failed",
             int((esp_timer_get_time() - started) / 1000));
    job->reply(result);
    job.reset();
    busy = false;
    vTaskDeleteWithCaps(nullptr);
}
}  // namespace

void PublishBookSource(const BookSource& source) {
    std::lock_guard<std::mutex> lock(source_mutex);
    latest_source = source;
}
BookSource LatestBookSource() {
    std::lock_guard<std::mutex> lock(source_mutex);
    return latest_source;
}

bool StartBookToolRequest(const BookSource& source, const cJSON* request, std::function<void(cJSON*)> reply) {
    if (busy.exchange(true)) return false;
    auto* job = new Job{source, cJSON_Duplicate(request, true), std::move(reply)};
    // 栈放 PSRAM（只读 SD、解压，不碰 NVS / flash），和打开书的后台任务一样
    if (!job->request || xTaskCreatePinnedToCoreWithCaps(Run, "book_tools", kStack, job, tskIDLE_PRIORITY + 1, nullptr, 0,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        cJSON_Delete(job->request);
        delete job;
        busy = false;
        return false;
    }
    return true;
}

}  // namespace valet
