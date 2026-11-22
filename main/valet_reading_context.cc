#include "valet_reading_context.h"
#include "valet_crypto/monocypher-ed25519.h"
#include <mutex>
#include <cstring>
#include <cstdio>
namespace valet {
namespace { std::mutex mutex; ReadingContext latest, selected; std::function<std::pair<int, std::string>()> expander; }
namespace {
std::string Clip(const std::string& text, size_t max) {
    if (text.size() <= max) return text;
    while (max && (static_cast<unsigned char>(text[max]) & 0xc0) == 0x80) --max;
    return text.substr(0, max);
}
}
namespace {
bool AsciiWord(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
// 句末：。！？；…  以及英文 . ! ? ;（后面跟空白）
size_t SentenceEnd(const std::string& s, size_t i) {
    static const char* const kMarks[] = {"\xe3\x80\x82", "\xef\xbc\x81", "\xef\xbc\x9f", "\xef\xbc\x9b", "\xe2\x80\xa6", "\xe2\x80\x9d"};
    for (const char* mark : kMarks) { const size_t n = std::strlen(mark); if (s.compare(i, n, mark) == 0) return i + n; }
    if ((s[i] == '.' || s[i] == '!' || s[i] == '?' || s[i] == ';') && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\n')) return i + 1;
    return 0;
}
// 留尾部 max 字节，从窗口里第一个句子边界之后开始（窗口前半段找不到就按字符边界截）
std::string KeepTail(const std::string& s, size_t max, bool& cut) {
    cut = s.size() > max;
    if (!cut) return s;
    size_t from = s.size() - max;
    while (from < s.size() && (static_cast<unsigned char>(s[from]) & 0xc0) == 0x80) ++from;
    for (size_t i = from; i < from + max / 2 && i < s.size(); ++i) if (const size_t end = SentenceEnd(s, i)) { from = end; break; }
    return s.substr(from);
}
// 留开头 max 字节，在窗口里最后一个句子边界处结束（窗口后半段找不到就按字符边界截）
std::string KeepHead(const std::string& s, size_t max, bool& cut) {
    cut = s.size() > max;
    if (!cut) return s;
    size_t to = max;
    while (to && (static_cast<unsigned char>(s[to]) & 0xc0) == 0x80) --to;
    size_t best = 0;
    for (size_t i = max / 2; i < to; ++i) if (const size_t end = SentenceEnd(s, i); end && end <= to) best = end;
    return s.substr(0, best ? best : to);
}
}  // namespace
std::string JoinPageLines(const std::vector<PageLine>& lines) {
    std::string out;
    for (const auto& line : lines) {
        if (line.text.empty()) continue;
        if (!out.empty()) {
            if (line.para_start) out += '\n';
            else if (static_cast<unsigned char>(out.back()) < 0x80 && out.back() != ' ' && AsciiWord(line.text.front())) out += ' ';   // 英文折行吃掉的空格
        }
        out += line.text;
    }
    return out;
}
std::string ComposeReadingText(const std::vector<PageLine>& prev, const std::vector<PageLine>& page,
                               const std::vector<PageLine>& next, size_t max, bool& truncated, size_t side_max) {
    truncated = false;
    const std::string body = JoinPageLines(page);
    std::string before, after;
    bool before_cut = false, after_cut = false;
    // 本页第一行不是段首：这一段从前一页开始，取前一页最后一个段首起的行（整页都在段中就全取）
    if (!page.empty() && !page.front().para_start && !prev.empty()) {
        size_t from = prev.size();
        while (from > 0 && !prev[from - 1].para_start) --from;
        bool whole = false;
        if (from > 0) --from; else whole = true;   // 前一页也没有段首：段落比一页还长，前面还有
        bool clipped = false;
        before = KeepTail(JoinPageLines({prev.begin() + from, prev.end()}), side_max, clipped);
        before_cut = whole || clipped;
    }
    // 后一页第一行不是段首：这一段延续到后一页，取到下一个段首之前
    if (!next.empty() && !next.front().para_start) {
        size_t to = 1;
        while (to < next.size() && !next[to].para_start) ++to;
        const bool runs_on = to == next.size();
        after = KeepHead(JoinPageLines({next.begin(), next.begin() + to}), side_max, after_cut);
        after_cut = after_cut || runs_on;
    }
    if (before.empty() && after.empty()) {
        if (body.size() <= max) return body;
        truncated = true; bool cut = false; return KeepHead(body, max, cut);
    }
    static const char kHeader[] = "（前后补全了被翻页截断的段落；【本页开始】到【本页结束】是这一页。）\n";
    static const char kStart[] = "【本页开始】", kEnd[] = "【本页结束】", kMore[] = "……";
    std::string head = std::string(kHeader) + (before.empty() ? "" : (before_cut ? std::string(kMore) : "") + before) + kStart;
    std::string tail = std::string(kEnd) + after + (after.empty() || !after_cut ? "" : kMore);
    if (head.size() + tail.size() + body.size() > max) {
        if (head.size() + tail.size() >= max) { truncated = body.size() > max; bool cut = false; return KeepHead(body, max, cut); }
        bool cut = false;
        const std::string clipped = KeepHead(body, max - head.size() - tail.size(), cut);
        truncated = cut;
        return head + clipped + tail;
    }
    return head + body + tail;
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
void SetReadingTextExpander(std::function<std::pair<int, std::string>()> f) { std::lock_guard<std::mutex> guard(mutex); expander = std::move(f); }
bool SelectBookContext() {
    std::function<std::pair<int, std::string>()> expand;
    int page = 0;
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (!latest.Valid()) return false;
        if (!latest.expanded) { expand = expander; page = latest.page; }
    }
    // 读前后两页在锁外做（要读 SD）；读完确认还是同一页再用
    auto wider = expand ? expand() : std::pair<int, std::string>{0, {}};
    std::lock_guard<std::mutex> guard(mutex);
    if (!latest.Valid()) return false;
    if (wider.first == page && page == latest.page && !wider.second.empty() && !latest.expanded) {
        latest.text = Clip(wider.second, 12000); latest.expanded = true;
    }
    selected = latest; return true;
}
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
