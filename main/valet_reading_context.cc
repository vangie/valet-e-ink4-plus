#include "valet_reading_context.h"
#include "valet_crypto/monocypher-ed25519.h"
#include <mutex>
#include <cstdio>
namespace valet {
namespace { std::mutex mutex; ReadingContext latest, selected; }
namespace {
std::string Clip(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    while (max && (static_cast<unsigned char>(text[max]) & 0xc0) == 0x80) --max;
    return text.substr(0, max);
}
}
void PublishReadingPage(const std::string& path, const std::string& title,
                        const std::string& chapter, int page, const std::string& text) {
    ReadingContext context;
    if (!path.empty() && page > 0 && !text.empty()) {
        uint8_t hash[64]; crypto_sha512(hash, reinterpret_cast<const uint8_t*>(path.data()), path.size());
        char id[33]{}; for (size_t i=0;i<16;++i) std::snprintf(id+i*2,3,"%02x",hash[i]);
        context.book_id = id; context.title = Clip(title.empty() ? "未命名书籍" : title, 240);
        context.chapter = Clip(chapter.empty() ? "正文" : chapter, 240);
        context.page = page; context.text = Clip(text, 12000); context.truncated = context.text.size() != text.size();
    }
    std::lock_guard<std::mutex> guard(mutex); latest = std::move(context);
}
ReadingContext LatestReadingPage() { std::lock_guard<std::mutex> guard(mutex); return latest; }
bool SelectBookContext() { std::lock_guard<std::mutex> guard(mutex); if (!latest.Valid()) return false; selected = latest; return true; }
void SelectStandaloneContext() { std::lock_guard<std::mutex> guard(mutex); selected = {}; }
ReadingContext SelectedReadingContext() { std::lock_guard<std::mutex> guard(mutex); return selected; }
std::string ContextStorageKey(const std::string& assistant_id) {
    const auto context = SelectedReadingContext();
    return assistant_id + ":" + (context.Valid() ? context.book_id : "standalone");
}
cJSON* ContextJson(const ReadingContext& context) {
    if (!context.Valid()) return cJSON_CreateNull();
    auto* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root,"bookId",context.book_id.c_str()); cJSON_AddStringToObject(root,"title",context.title.c_str());
    cJSON_AddStringToObject(root,"chapter",context.chapter.c_str()); cJSON_AddNumberToObject(root,"page",context.page);
    cJSON_AddStringToObject(root,"text",context.text.c_str()); cJSON_AddBoolToObject(root,"truncated",context.truncated); return root;
}
}
